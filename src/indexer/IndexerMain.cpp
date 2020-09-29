//===--- IndexerMain.cpp -----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// clangd-indexer is a tool to gather index data (symbols, xrefs) from source.
//
//===----------------------------------------------------------------------===//

#include "index/IndexAction.h"
#include "index/LSIFSerialization.h"
#include "index/Merge.h"
#include "index/Ref.h"
#include "index/Serialization.h"
#include "index/Symbol.h"
#include "index/SymbolCollector.h"
#include "clang/Index/IndexingOptions.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Execution.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Signals.h"
#include <llvm-10/llvm/ADT/SmallVector.h>
#include <llvm-10/llvm/Support/FileSystem.h>

// static llvm::cl::opt<IndexFileFormat> Format(
//     "format", llvm::cl::desc("Format of the index to be written"),
//     llvm::cl::values(
//         clEnumValN(IndexFileFormat::YAML, "yaml", "human-readable YAML format"),
//         clEnumValN(IndexFileFormat::RIFF, "binary", "binary RIFF format"),
//         clEnumValN(IndexFileFormat::LSIF, "lsif", "exportable LSIF format")),
//     llvm::cl::init(IndexFileFormat::RIFF));

static llvm::cl::opt<std::string> ProjectRoot(
    "project-root", llvm::cl::desc("Absolute path to root directory of project being indexed"),
    llvm::cl::init(""));

static llvm::cl::opt<bool> Debug(
    "debug",
    llvm::cl::desc("Enable verbose debug output."),
    llvm::cl::init(false));

class IndexActionFactory : public clang::tooling::FrontendActionFactory {
public:
  IndexActionFactory(clang::clangd::IndexFileIn &Result) : Result(Result) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    clang::clangd::SymbolCollector::Options Opts;
    Opts.CountReferences = true;
    Opts.CollectMainFileSymbols = true;
    Opts.StoreAllDocumentation = true;
    clang::index::IndexingOptions IndexOpts;
    IndexOpts.IndexFunctionLocals = true;
    IndexOpts.IndexParametersInDeclarations = true;
    IndexOpts.IndexImplicitInstantiation = true;
    IndexOpts.IndexMacrosInPreprocessor = true;
    IndexOpts.SystemSymbolFilter = clang::index::IndexingOptions::SystemSymbolFilterKind::All;
    return createStaticIndexingAction(
        Opts,
        IndexOpts,
        [&](clang::clangd::SymbolSlab S) {
          // Merge as we go.
          std::lock_guard<std::mutex> Lock(SymbolsMu);
          for (const auto &Sym : S) {
            if (const auto *Existing = Symbols.find(Sym.ID))
              Symbols.insert(mergeSymbol(*Existing, Sym));
            else
              Symbols.insert(Sym);
          }
        },
        [&](clang::clangd::RefSlab S) {
          std::lock_guard<std::mutex> Lock(SymbolsMu);
          for (const auto &Sym : S) {
            // Deduplication happens during insertion.
            for (const auto &Ref : Sym.second)
              Refs.insert(Sym.first, Ref);
          }
        },
        [&](clang::clangd::RelationSlab S) {
          std::lock_guard<std::mutex> Lock(SymbolsMu);
          for (const auto &R : S) {
            Relations.insert(R);
          }
        },
        /*IncludeGraphCallback=*/nullptr);
  }

  // Awkward: we write the result in the destructor, because the executor
  // takes ownership so it's the easiest way to get our data back out.
  ~IndexActionFactory() {
    Result.Symbols = std::move(Symbols).build();
    Result.Refs = std::move(Refs).build();
    Result.Relations = std::move(Relations).build();
  }

private:
  clang::clangd::IndexFileIn &Result;
  std::mutex SymbolsMu;
  clang::clangd::SymbolSlab::Builder Symbols;
  clang::clangd::RefSlab::Builder Refs;
  clang::clangd::RelationSlab::Builder Relations;
};

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  const char *Overview = R"(
  Creates an index of symbol information etc in a whole project.

  Example usage for a project using CMake compile commands:

  $ lsif-clang --executor=all-TUs compile_commands.json > clangd.dex

  Example usage for file sequence index without flags:

  $ lsif-clang File1.cpp File2.cpp ... FileN.cpp > clangd.dex
  )";

  auto Executor = clang::tooling::createExecutorFromCommandLineArgs(
      argc, argv, llvm::cl::GeneralCategory, Overview);

  if (!Executor) {
    llvm::errs() << llvm::toString(Executor.takeError()) << "\n";
    return 1;
  }

  // Collect symbols found in each translation unit, merging as we go.
  clang::clangd::IndexFileIn Data;
  auto Err = Executor->get()->execute(
      std::make_unique<IndexActionFactory>(Data),
      clang::tooling::getStripPluginsAdjuster());
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
  }

  // Emit collected data.
  clang::clangd::IndexFileOut Out(Data);
  Out.Format = clang::clangd::IndexFileFormat::LSIF;
  if (ProjectRoot == "") {
    llvm::SmallString<128> CurrentPath;
    llvm::sys::fs::current_path(CurrentPath);
    Out.ProjectRoot = std::string("file://") + CurrentPath.c_str();
  } else {
    Out.ProjectRoot = ProjectRoot;
  }
  Out.Debug = Debug;
  writeLSIF(Out, llvm::outs());
  return 0;
}