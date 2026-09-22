// b2t2 - the B-2 Tier 2 execution driver.
//
// WHY THIS FILE EXISTS:
// The T2 execution driver is the end-to-end surface of the optimizing tier:
// parse an RBC text program, verify it (the hard gate), build every method's
// sea-of-nodes IR graph (passes::buildGraph), optionally run the optimization
// pipeline (passes::runEarlyCleanup + passes::runInlining), lower each graph
// to x86-64 machine code (codegen::lowerOnly), install the CompiledCode on a
// Tier1 engine (which provides the helper-call dispatch, W^X activation, and
// deopt-to-T0 path), and execute the entry method. The result is reported
// with EXACTLY b2run's launcher shape - the differential contract (Rule 36
// form) is that b2run and b2t2 are observationally indistinguishable on every
// program both can run, in the default (no-opt) configuration.
//
// WHY THIS IS A SEPARATE TOOL FROM b2graph --exec:
// b2graph is the IR builder's debug surface (parse -> build -> print). Its
// --exec mode happens to wire the T2 path end-to-end, but the surface is
// tuned for lowering review, not for execution. b2t2 is the T2 driver
// surface: it mirrors b2jit's role for T1 (parse -> compile -> run -> report
// stats). The two tools share the T2 lowering code (codegen::lowerOnly)
// but serve different audiences. b2t2's default is NO optimization (matching
// T0 byte-for-byte); -O opts in to the optimization pipeline (currently
// known to diverge from T0 on some inputs; see MSG-20260918-006).
//
// Usage:
//   b2t2 program.rbc [--entry NAME DESC] [--stats] [--quiet] [--code NAME]
//                    [-O] [--inline] [--pea] [--pgo]
//
//   --entry NAME DESC   entry method (default: main; descriptor inferred
//                       like b2run: ()V if present, else the String[] form)
//   --stats             print tier counters to stderr after the run
//   --quiet             suppress the "[b2t2] ..." status lines
//   --code NAME         dump the compiled code of every method whose name
//                       contains NAME (the golden/inspection path)
//   -O, --optimize      run the early-cleanup + GVN + SCCP pipeline before
//                       lowering (KNOWN BUG: diverges from T0 on some
//                       inputs, e.g. strings_intern.rbc; see
//                       MSG-20260918-006; default OFF for differential safety)
//   --inline            run the ICDG inline engine before lowering
//   --pea               run the CM-PEA engine before lowering
//   --pgo               run the program in T0 FIRST (training run), snapshot
//                       the per-site dispatch profile, feed it to the inline
//                       engine (implies --inline)
//
// Exit status: 0 = normal return, 1 = uncaught Java exception or failure.

#include <csignal>
#include <ucontext.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DispatchProfileSnapshot.h"
#include "b2/codegen/T2Lowering.h"
#include "b2/codegen/Tier1.h"
#include "b2/interp/Interp.h"
#include "b2/ir/Verifier.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/passes/Inline.h"
#include "b2/passes/Passes.h"
#include "b2/pipeline/DependencyIndex.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

void segvHandler(int, siginfo_t* info, void* ctx) {
  const auto* uc = static_cast<const ucontext_t*>(ctx);
  std::fprintf(stderr, "[b2t2] SIGSEGV addr=%p rip=%llx rbp=%llx rdi=%llx\n",
               info->si_addr,
               (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
               (unsigned long long)uc->uc_mcontext.gregs[REG_RBP],
               (unsigned long long)uc->uc_mcontext.gregs[REG_RDI]);
  _exit(139);
}

[[nodiscard]] bool readFile(const std::filesystem::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

void reportUncaught(b2::interp::Runtime& rt, b2::interp::ObjRef exc) {
  std::string cls(rt.classNameOf(exc));
  for (auto& c : cls) {
    if (c == '/') c = '.';
  }
  std::string msg(rt.exceptionMessage(exc));
  if (msg.empty()) {
    std::fprintf(stderr, "Exception in thread \"main\" %s\n", cls.c_str());
  } else {
    std::fprintf(stderr, "Exception in thread \"main\" %s: %s\n", cls.c_str(),
                 msg.c_str());
  }
}

void reportStats(const b2::codegen::Tier1Stats& s,
                 std::uint32_t lowered, std::uint32_t refused) {
  std::fprintf(stderr,
               "[b2t2] lowered=%u refused=%u attempts=%u ok=%u "
               "deopts(trap=%u callExc=%u guard=%u) t0Fallback=%u "
               "entries=%llu helperCalls=%llu codeBytes=%llu\n",
               lowered, refused, s.compile_attempts, s.compile_ok,
               s.deopt_trap, s.deopt_call_exception, s.deopt_guard,
               s.t0_fallback_executions,
               static_cast<unsigned long long>(s.t1_entries),
               static_cast<unsigned long long>(s.helper_calls),
               static_cast<unsigned long long>(s.code_bytes));
}

// The T0 training run (the --pgo half; mirrors b2graph's trainAndSnapshot).
[[nodiscard]] const b2::passes::DispatchProfile*
trainAndSnapshot(const b2::rbc::Program& prog,
                 b2::passes::DispatchProfile& storage) {
  b2::interp::Interpreter interp(prog, b2::interp::InterpConfig{});
  std::string entryDesc;
  if (prog.find("main", "()V") != nullptr) {
    entryDesc = "()V";
  } else if (prog.find("main", "([Ljava/lang/String;)V") != nullptr) {
    entryDesc = "([Ljava/lang/String;)V";
  } else {
    for (const b2::rbc::Method& m : prog.methods) {
      if (m.name == "main" && b2::rbc::paramCount(m.descriptor) == 0) {
        entryDesc = m.descriptor;
        break;
      }
    }
  }
  std::vector<b2::interp::Value> args;
  if (entryDesc == "([Ljava/lang/String;)V") {
    args.push_back(b2::interp::Value::refVal(
        interp.runtime().newRefArray(interp.runtime().stringClass(), 0)));
  }
  if (!entryDesc.empty()) {
    (void)interp.run("main", entryDesc, args);
  }
  b2::passes::snapshotDispatchProfile(interp, storage);
  return &storage;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: b2t2 program.rbc [--entry NAME DESC] [--stats] "
                 "[--quiet] [--code NAME] [-O] [--inline] [--pea] [--pgo]\n");
    return 1;
  }

  std::filesystem::path path;
  std::string entryName = "main";
  std::string entryDesc;
  bool stats = false, quiet = false;
  std::string codeFilter;
  bool optimize = false;
  bool inl = false;
  bool pgo = false;
  bool pea = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--entry" && i + 2 < argc) {
      entryName = argv[++i];
      entryDesc = argv[++i];
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--code" && i + 1 < argc) {
      codeFilter = argv[++i];
    } else if (arg == "-O" || arg == "--optimize") {
      optimize = true;
    } else if (arg == "--inline" || arg == "-i") {
      inl = true;
    } else if (arg == "--pea") {
      pea = true;
    } else if (arg == "--pgo") {
      pgo = true;
      inl = true;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::fprintf(stderr, "[b2t2] unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: b2t2 program.rbc\n");
    return 1;
  }

  if (getenv("B2T2_SEGV") != nullptr) {
    struct sigaction sa{};
    sa.sa_sigaction = segvHandler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
  }
  std::string text;
  if (!readFile(path, text)) {
    std::fprintf(stderr, "[b2t2] cannot read %s\n", path.string().c_str());
    return 1;
  }

  auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    std::fprintf(stderr, "[b2t2] parse error at byte %u: %s\n",
                 parsed.error().offset, parsed.error().message.c_str());
    return 1;
  }

  // Hard-gate verify (the same discipline as T0/b2run/b2jit).
  for (const b2::rbc::Method& m : parsed->methods) {
    const b2::rbc::VerifyResult vr = b2::rbc::verify(m);
    if (!vr.ok) {
      std::fprintf(stderr, "[b2t2] RBC verification failed for %s:\n",
                   m.name.c_str());
      for (const b2::rbc::VerifyDiag& d : vr.diags) {
        std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
      }
      return 1;
    }
  }

  // The T0 training run (--pgo): snapshot the per-site dispatch profile.
  b2::passes::DispatchProfile profileStorage;
  const b2::passes::DispatchProfile* profile = nullptr;
  if (pgo) {
    profile = trainAndSnapshot(*parsed, profileStorage);
  }

  // The runtime DependencyIndex (partial deopt v0.3). When the inline pass
  // creates a GuardInline guard + ClassHierarchy dependency, it calls
  // depIndex.record() to register the association. The runtime can later
  // call depIndex.invalidate(dep) when a new subclass is loaded to get
  // the dirty set for the partial deopt path.
  b2::pipeline::DependencyIndex depIndex;

  // Build the Tier1 engine (provides the helper-call dispatch, W^X activation,
  // and deopt-to-T0 path; the T2 lowering installs code into its cache).
  b2::codegen::Tier1 engine(*parsed, b2::codegen::Tier1Config{});

  // Phase 1: lower every method via the T2 path (RBC -> IR -> machine code).
  // Methods that refuse any stage (build, IR-verify, lower) are silently
  // skipped here; the engine will fall back to T1 baseline or T0 for them
  // when invoked (Rule 96: the plan is a cache, not a correctness claim).
  std::uint32_t lowered = 0, refused = 0;
  std::vector<std::string> refuseReasons;
  for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
    const b2::rbc::Method& m = parsed->methods[i];
    b2::passes::ProgramCalleeSource resolver(*parsed);
    b2::ir::Graph g;
    const b2::passes::BuildResult br = b2::passes::buildGraph(
        m, resolver, g, static_cast<b2::ir::MethodId>(i));
    if (br.hasErrors()) {
      std::string reason = "buildGraph: " + (br.diags.empty() ? "" : br.diags[0].message);
      refuseReasons.push_back(m.name + ": " + reason);
      ++refused;
      continue;
    }
    if (inl) {
      b2::passes::InlineConfig icfg;
      icfg.profile = profile;
      icfg.depIndex = &depIndex;
      const b2::passes::InlineResult ir =
          b2::passes::runInlining(g, resolver, icfg);
      if (!ir.ok) {
        refuseReasons.push_back(m.name + ": runInlining failed");
        ++refused;
        continue;
      }
    }
    if (pea) {
      (void)b2::passes::runPartialEscapeAnalysis(g);
    }
    if (optimize) {
      const b2::passes::PassResult pr = b2::passes::runEarlyCleanup(g);
      if (!pr.ok) {
        refuseReasons.push_back(m.name + ": runEarlyCleanup failed");
        ++refused;
        continue;
      }
    }
    const b2::ir::VerifyResult irv = b2::ir::verify(g);
    if (irv.hasErrors()) {
      std::string reason = "IR verify: " + (irv.diags.empty() ? "" : irv.diags[0].message);
      refuseReasons.push_back(m.name + ": " + reason);
      ++refused;
      continue;
    }
    std::string refusalReason;
    auto cc = b2::codegen::lowerOnly(
        g, *parsed, m, static_cast<std::uint32_t>(i),
        engine.interp().runtime(), &refusalReason);
    if (!cc) {
      refuseReasons.push_back(m.name + ": lowerOnly: " + refusalReason);
      ++refused;
      continue;
    }
    engine.installCompiledCode(std::move(cc));
    ++lowered;
  }

  if (!quiet && (refused > 0 || stats)) {
    for (const auto& r : refuseReasons) {
      std::fprintf(stderr, "[b2t2] refused %s\n", r.c_str());
    }
  }

  // Phase 2: resolve the entry method (mirrors b2run's entry inference).
  if (entryDesc.empty()) {
    entryDesc = "()V";
    if (!parsed->find(entryName, entryDesc)) {
      entryDesc = "([Ljava/lang/String;)V";
    }
  }
  std::vector<b2::interp::Value> args;
  if (entryDesc == "([Ljava/lang/String;)V") {
    auto arr = engine.interp().runtime().newRefArray(
        engine.interp().runtime().stringClass(), 0);
    args.push_back(b2::interp::Value::refVal(arr));
  }

  // Phase 3: execute the entry method on the engine. The engine looks up the
  // installed T2 code first; if missing, falls back to T1 baseline then T0
  // (Rule 96: the plan is a cache, not a correctness claim).
  const b2::codegen::Tier1RunResult r =
      engine.run(entryName, entryDesc, args);

  // Program-visible output first (Java ordering discipline, like b2run).
  // Flush stdout AND stderr buffers (the runtime accumulates both; b2run
  // flushes both; b2graph --exec only flushed stdout, which is a known gap
  // fixed here).
  std::fwrite(engine.interp().runtime().stdout().data(), 1,
              engine.interp().runtime().stdout().size(), stdout);
  std::fflush(stdout);
  std::fwrite(engine.interp().runtime().stderr().data(), 1,
              engine.interp().runtime().stderr().size(), stderr);

  switch (r.status) {
    case b2::codegen::Tier1Status::Returned:
      break;
    case b2::codegen::Tier1Status::Threw:
      if (!quiet) {
        reportUncaught(engine.interp().runtime(), r.exception);
      }
      break;
    case b2::codegen::Tier1Status::VerifyFailed:
      if (!quiet) {
        std::fprintf(stderr, "[b2t2] verification failed (%zu diagnostics):\n",
                     r.verify_diags.size());
        for (const auto& d : r.verify_diags) {
          std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
        }
      }
      break;
    case b2::codegen::Tier1Status::NoSuchMethod:
      if (!quiet) {
        std::fprintf(stderr, "[b2t2] entry method not found\n");
      }
      break;
  }

  if (stats) {
    reportStats(engine.stats(), lowered, refused);
  }
  if (!codeFilter.empty()) {
    for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
      if (parsed->methods[i].name.find(codeFilter) == std::string::npos) {
        continue;
      }
      const b2::codegen::CompiledCode* cc =
          engine.codeFor(static_cast<std::uint32_t>(i));
      if (cc == nullptr) {
        std::fprintf(stderr, "[%s] %s%s: not compiled\n", codeFilter.c_str(),
                     parsed->methods[i].name.c_str(),
                     parsed->methods[i].descriptor.c_str());
        continue;
      }
      std::fprintf(stderr, "[%s] %s%s compiled (T2):\n%s", codeFilter.c_str(),
                   parsed->methods[i].name.c_str(),
                   parsed->methods[i].descriptor.c_str(),
                   b2::codegen::dumpCode(*cc).c_str());
    }
  }

  return r.status == b2::codegen::Tier1Status::Returned ? 0 : 1;
}
