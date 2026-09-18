// Standalone debug tool: lower a method via T2, execute, print stdout buffer
// state. Built outside the cmake tree (single TU + b2::codegen + b2::passes
// + b2::interp + b2::rbc + b2::ir). Used to diagnose the strings_intern T2
// no-output bug.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "b2/codegen/T2Lowering.h"
#include "b2/codegen/Tier1.h"
#include "b2/interp/Interp.h"
#include "b2/ir/Printer.h"
#include "b2/ir/Verifier.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/passes/Inline.h"
#include "b2/passes/Passes.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s file.rbc\n", argv[0]); return 1; }
  std::ifstream in(argv[1]);
  std::stringstream ss; ss << in.rdbuf();
  auto parsed = b2::rbc::parseRbcText(ss.str());
  if (!parsed) {
    std::fprintf(stderr, "parse error: %s\n", parsed.error().message.c_str());
    return 1;
  }

  b2::codegen::Tier1 engine(*parsed, b2::codegen::Tier1Config{});
  std::uint32_t lowered = 0, refused = 0;
  for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
    const b2::rbc::Method& m = parsed->methods[i];
    const b2::rbc::VerifyResult vr = b2::rbc::verify(m);
    if (vr.hasErrors()) { std::fprintf(stderr, "rbc verify failed for %s\n", m.name.c_str()); ++refused; continue; }
    b2::passes::ProgramCalleeSource resolver(*parsed);
    b2::ir::Graph g;
    const b2::passes::BuildResult br = b2::passes::buildGraph(m, resolver, g, static_cast<b2::ir::MethodId>(i));
    if (br.hasErrors()) {
      std::fprintf(stderr, "buildGraph failed for %s:\n", m.name.c_str());
      for (const auto& d : br.diags) std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
      ++refused; continue;
    }
    const b2::ir::VerifyResult irv = b2::ir::verify(g);
    if (irv.hasErrors()) {
      std::fprintf(stderr, "IR verify failed for %s: %s\n", m.name.c_str(),
                   irv.diags.empty() ? "" : irv.diags[0].message.c_str());
      ++refused; continue;
    }
    // Match b2graph --exec: run inliner + early-cleanup pipeline before lowering.
    if (argc >= 3 && std::string_view(argv[2]) == "--opt") {
      b2::passes::InlineConfig icfg;
      (void)b2::passes::runInlining(g, resolver, icfg);
      (void)b2::passes::runEarlyCleanup(g);
      const b2::ir::VerifyResult irv2 = b2::ir::verify(g);
      if (irv2.hasErrors()) {
        std::fprintf(stderr, "post-opt IR verify failed for %s\n", m.name.c_str());
        ++refused; continue;
      }
      // Dump post-opt graph for inspection.
      std::fputs(b2::ir::print(g).c_str(), stdout);
      std::fflush(stdout);
    }
    std::string refusalReason;
    auto cc = b2::codegen::lowerOnly(g, *parsed, m, static_cast<std::uint32_t>(i),
                                     engine.interp().runtime(), &refusalReason);
    if (!cc) { std::fprintf(stderr, "lowerOnly refused for %s: %s\n", m.name.c_str(), refusalReason.c_str()); ++refused; continue; }
    engine.installCompiledCode(std::move(cc));
    ++lowered;
  }

  std::fprintf(stderr, "[debug] lowered=%u refused=%u\n", lowered, refused);
  std::fprintf(stderr, "[debug] pre-run  stdout_buf_size=%zu stderr_buf_size=%zu\n",
               engine.interp().runtime().stdout().size(),
               engine.interp().runtime().stderr().size());

  const b2::codegen::Tier1RunResult r = engine.run("main", "()V", {});

  std::fprintf(stderr, "[debug] post-run stdout_buf_size=%zu stderr_buf_size=%zu\n",
               engine.interp().runtime().stdout().size(),
               engine.interp().runtime().stderr().size());
  std::fprintf(stderr, "[debug] status=%d helper_calls=%llu deopts=%u/%u/%u t0_fallback=%u\n",
               static_cast<int>(r.status),
               static_cast<unsigned long long>(engine.stats().helper_calls),
               engine.stats().deopt_trap, engine.stats().deopt_call_exception,
               engine.stats().deopt_guard, engine.stats().t0_fallback_executions);

  // Dump buffer raw bytes (hex)
  const std::string& out = engine.interp().runtime().stdout();
  std::fprintf(stderr, "[debug] stdout hex:");
  for (std::size_t i = 0; i < out.size() && i < 64; ++i) {
    std::fprintf(stderr, " %02x", static_cast<unsigned char>(out[i]));
  }
  std::fprintf(stderr, "\n[debug] stdout str: ");
  for (char c : out) {
    if (c >= 32 && c < 127) std::fputc(c, stderr);
    else std::fprintf(stderr, "\\x%02x", static_cast<unsigned char>(c));
  }
  std::fputc('\n', stderr);

  // Print actual stdout buffer content
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
  return r.status == b2::codegen::Tier1Status::Returned ? 0 : 1;
}
