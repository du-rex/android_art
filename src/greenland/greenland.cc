/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "greenland.h"

#include "gbc_context.h"
#include "gbc_function.h"
#include "target_codegen_machine.h"
#include "target_registry.h"

#include "class_linker.h"
#include "compiler.h"
#include "oat_compilation_unit.h"
#include "stl_util.h"
#include "utils.h"

#include <vector>

#include <llvm/InitializePasses.h>
#include <llvm/Module.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/Threading.h>

namespace art {
namespace greenland {

// Forward declarations
#define LLVM_TARGET(TargetName) void Initialize##TargetName##CodeGenMachine();
#include <llvm/Config/Targets.def>

#define LLVM_TARGET(TargetName) \
    void Initialize##TargetName##InvokeStubCompiler();
#include <llvm/Config/Targets.def>

} // namespace greeland
} // namespace art

namespace {

pthread_once_t greenland_initialized = PTHREAD_ONCE_INIT;

void InitializeAllCodeGenMachines() {
#define LLVM_TARGET(TargetName) \
  art::greenland::Initialize##TargetName##CodeGenMachine();
#include <llvm/Config/Targets.def>
}

void InitializeAllInvokeStubCompilers() {
#define LLVM_TARGET(TargetName) \
    art::greenland::Initialize##TargetName##InvokeStubCompiler();
#include <llvm/Config/Targets.def>
}

void InitializeGreenland() {
  // Initialize LLVM internal data structure for multithreading
  llvm::llvm_start_multithreaded();

  // Initialize passes
  llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();

  llvm::initializeCore(registry);
  llvm::initializeScalarOpts(registry);

  // Run vectorization passes when our backend supports vector type
  //llvm::initializeVectorization(registry);

  // DexLang operates on an llvm::Function and never runs IPO and IPA
  //llvm::initializeIPO(registry);
  //llvm::initializeIPA(registry);

  llvm::initializeAnalysis(registry);
  llvm::initializeTransformUtils(registry);
  llvm::initializeInstCombine(registry);

  InitializeAllCodeGenMachines();
  InitializeAllInvokeStubCompilers();

  return;
}

} // anonymous namespace

namespace art {
namespace greenland {

Greenland::Greenland(art::Compiler& compiler)
    : compiler_(compiler), codegen_machine_(NULL),
      lock_("greenland_compiler_lock"), cur_gbc_ctx_(NULL) {
  // Initialize Greenland
  pthread_once(&greenland_initialized, InitializeGreenland);

  codegen_machine_ =
      TargetCodeGenMachine::Create(compiler_.GetInstructionSet());
  DCHECK(codegen_machine_ != NULL);
  return;
}

Greenland::~Greenland() {
  cur_gbc_ctx_->DecRef();
  delete codegen_machine_;
}

CompiledMethod* Greenland::Compile(OatCompilationUnit& cunit) {
  MutexLock GUARD(lock_);

  // Dex to LLVM IR
  GBCContext& gbc_ctx = GetGBCContext();

  UniquePtr<DexLang> dex_lang(new DexLang(gbc_ctx.GetDexLangContext(),
                                          compiler_, cunit));

  llvm::Function* func = dex_lang->Build();

  if (func == NULL) {
    LOG(FATAL) << "Failed to run dexlang on "
               << PrettyMethod(cunit.GetDexMethodIndex(), *cunit.GetDexFile());
    return NULL;
  }

  func->dump();

  // NOTE: From statistic, the bitcode size is 4.5 times bigger than the
  // Dex file.  Besides, we have to convert the code unit into bytes.
  // Thus, we got our magic number 9.
  gbc_ctx.AddMemUsageApproximation(
      cunit.GetCodeItem()->insns_size_in_code_units_ * 900);

  GBCFunction gbc_func(gbc_ctx, *func, cunit);

  UniquePtr<CompiledMethod> result(codegen_machine_->Run(compiler_, gbc_func));

  // gbc_ctx was no longer needed
  gbc_ctx.DecRef();

  return result.release();
}

GBCContext& Greenland::GetGBCContext() {
  //MutexLock GUARD(lock_);

  ResetGBCContextIfThresholdReached();

  if (cur_gbc_ctx_ == NULL) {
    cur_gbc_ctx_ = new GBCContext();
  }
  CHECK(cur_gbc_ctx_ != NULL);

  return cur_gbc_ctx_->IncRef();
}

void Greenland::ResetGBCContextIfThresholdReached() {
  lock_.AssertHeld();

  if (cur_gbc_ctx_ == NULL) {
    return;
  }

  if (cur_gbc_ctx_->IsMemUsageThresholdReached()) {
    cur_gbc_ctx_->DecRef();
    cur_gbc_ctx_ = NULL;
  }
  return;
}

} // namespace greenland
} // namespace art

inline static art::greenland::Greenland* ContextOf(art::Compiler& compiler) {
  void *compiler_context = compiler.GetCompilerContext();
  CHECK(compiler_context != NULL);
  return reinterpret_cast<art::greenland::Greenland*>(compiler_context);
}

extern "C" void ArtInitCompilerContext(art::Compiler& compiler) {
  CHECK(compiler.GetCompilerContext() == NULL);
  compiler.SetCompilerContext(new art::greenland::Greenland(compiler));
  return;
}

extern "C" art::CompiledMethod* ArtCompileMethod(art::Compiler& compiler,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags, uint32_t method_idx,
                                                 const art::ClassLoader* class_loader,
                                                 const art::DexFile& dex_file)
{
  art::ClassLinker *class_linker = art::Runtime::Current()->GetClassLinker();
  art::DexCache *dex_cache = class_linker->FindDexCache(dex_file);

  art::OatCompilationUnit cunit(
    class_loader, class_linker, dex_file, *dex_cache, code_item,
    method_idx, access_flags);

  return ContextOf(compiler)->Compile(cunit);
}

extern "C" art::CompiledInvokeStub* ArtCreateInvokeStub(art::Compiler& compiler,
                                                        bool is_static,
                                                        const char* shorty,
                                                        uint32_t shorty_len) {
  art::greenland::TargetRegistry::CreateInvokeStubFn compiler_fn =
      art::greenland::TargetRegistry::GetInvokeStubCompiler(compiler.GetInstructionSet());
  CHECK(compiler_fn != NULL);
  return (*compiler_fn)(compiler, is_static, shorty, shorty_len);
}
