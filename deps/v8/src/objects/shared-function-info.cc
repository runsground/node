// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/objects/shared-function-info.h"

#include "src/ast/ast.h"
#include "src/ast/scopes.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/compiler.h"
#include "src/diagnostics/code-tracer.h"
#include "src/objects/shared-function-info-inl.h"
#include "src/strings/string-builder-inl.h"

namespace v8 {
namespace internal {

V8_EXPORT_PRIVATE constexpr Smi SharedFunctionInfo::kNoSharedNameSentinel;

uint32_t SharedFunctionInfo::Hash() {
  // Hash SharedFunctionInfo based on its start position and script id. Note: we
  // don't use the function's literal id since getting that is slow for compiled
  // funcitons.
  int start_pos = StartPosition();
  int script_id = script().IsScript() ? Script::cast(script()).id() : 0;
  return static_cast<uint32_t>(base::hash_combine(start_pos, script_id));
}

void SharedFunctionInfo::Init(ReadOnlyRoots ro_roots, int unique_id) {
  DisallowHeapAllocation no_allocation;

  // Set the function data to the "illegal" builtin. Ideally we'd use some sort
  // of "uninitialized" marker here, but it's cheaper to use a valid buitin and
  // avoid having to do uninitialized checks elsewhere.
  set_builtin_id(Builtins::kIllegal);

  // Set the name to the no-name sentinel, this can be updated later.
  set_name_or_scope_info(SharedFunctionInfo::kNoSharedNameSentinel,
                         kReleaseStore, SKIP_WRITE_BARRIER);

  // Generally functions won't have feedback, unless they have been created
  // from a FunctionLiteral. Those can just reset this field to keep the
  // SharedFunctionInfo in a consistent state.
  set_raw_outer_scope_info_or_feedback_metadata(ro_roots.the_hole_value(),
                                                SKIP_WRITE_BARRIER);
  set_script_or_debug_info(ro_roots.undefined_value(), SKIP_WRITE_BARRIER);
  set_function_literal_id(kFunctionLiteralIdInvalid);
#if V8_SFI_HAS_UNIQUE_ID
  set_unique_id(unique_id);
#endif

  // Set integer fields (smi or int, depending on the architecture).
  set_length(0);
  set_internal_formal_parameter_count(0);
  set_expected_nof_properties(0);
  set_raw_function_token_offset(0);

  // All flags default to false or 0, except ConstructAsBuiltinBit just because
  // we're using the kIllegal builtin.
  set_flags(ConstructAsBuiltinBit::encode(true));
  set_flags2(0);

  UpdateFunctionMapIndex();

  clear_padding();
}

Code SharedFunctionInfo::GetCode() const {
  // ======
  // NOTE: This chain of checks MUST be kept in sync with the equivalent CSA
  // GetSharedFunctionInfoCode method in code-stub-assembler.cc.
  // ======

  Isolate* isolate = GetIsolate();
  Object data = function_data(kAcquireLoad);
  if (data.IsSmi()) {
    // Holding a Smi means we are a builtin.
    DCHECK(HasBuiltinId());
    return isolate->builtins()->builtin(builtin_id());
  } else if (data.IsBytecodeArray()) {
    // Having a bytecode array means we are a compiled, interpreted function.
    DCHECK(HasBytecodeArray());
    return isolate->builtins()->builtin(Builtins::kInterpreterEntryTrampoline);
  } else if (data.IsAsmWasmData()) {
    // Having AsmWasmData means we are an asm.js/wasm function.
    DCHECK(HasAsmWasmData());
    return isolate->builtins()->builtin(Builtins::kInstantiateAsmJs);
  } else if (data.IsUncompiledData()) {
    // Having uncompiled data (with or without scope) means we need to compile.
    DCHECK(HasUncompiledData());
    return isolate->builtins()->builtin(Builtins::kCompileLazy);
  } else if (data.IsFunctionTemplateInfo()) {
    // Having a function template info means we are an API function.
    DCHECK(IsApiFunction());
    return isolate->builtins()->builtin(Builtins::kHandleApiCall);
  } else if (data.IsWasmExportedFunctionData()) {
    // Having a WasmExportedFunctionData means the code is in there.
    DCHECK(HasWasmExportedFunctionData());
    return wasm_exported_function_data().wrapper_code();
  } else if (data.IsInterpreterData()) {
    Code code = InterpreterTrampoline();
    DCHECK(code.IsCode());
    DCHECK(code.is_interpreter_trampoline_builtin());
    return code;
  } else if (data.IsWasmJSFunctionData()) {
    return wasm_js_function_data().wrapper_code();
  } else if (data.IsWasmCapiFunctionData()) {
    return wasm_capi_function_data().wrapper_code();
  }
  UNREACHABLE();
}

WasmExportedFunctionData SharedFunctionInfo::wasm_exported_function_data()
    const {
  DCHECK(HasWasmExportedFunctionData());
  return WasmExportedFunctionData::cast(function_data(kAcquireLoad));
}

WasmJSFunctionData SharedFunctionInfo::wasm_js_function_data() const {
  DCHECK(HasWasmJSFunctionData());
  return WasmJSFunctionData::cast(function_data(kAcquireLoad));
}

WasmCapiFunctionData SharedFunctionInfo::wasm_capi_function_data() const {
  DCHECK(HasWasmCapiFunctionData());
  return WasmCapiFunctionData::cast(function_data(kAcquireLoad));
}

SharedFunctionInfo::ScriptIterator::ScriptIterator(Isolate* isolate,
                                                   Script script)
    : ScriptIterator(handle(script.shared_function_infos(), isolate)) {}

SharedFunctionInfo::ScriptIterator::ScriptIterator(
    Handle<WeakFixedArray> shared_function_infos)
    : shared_function_infos_(shared_function_infos), index_(0) {}

SharedFunctionInfo SharedFunctionInfo::ScriptIterator::Next() {
  while (index_ < shared_function_infos_->length()) {
    MaybeObject raw = shared_function_infos_->Get(index_++);
    HeapObject heap_object;
    if (!raw->GetHeapObject(&heap_object) || heap_object.IsUndefined()) {
      continue;
    }
    return SharedFunctionInfo::cast(heap_object);
  }
  return SharedFunctionInfo();
}

void SharedFunctionInfo::ScriptIterator::Reset(Isolate* isolate,
                                               Script script) {
  shared_function_infos_ = handle(script.shared_function_infos(), isolate);
  index_ = 0;
}

void SharedFunctionInfo::SetScript(ReadOnlyRoots roots,
                                   HeapObject script_object,
                                   int function_literal_id,
                                   bool reset_preparsed_scope_data) {
  DisallowHeapAllocation no_gc;

  if (script() == script_object) return;

  if (reset_preparsed_scope_data && HasUncompiledDataWithPreparseData()) {
    ClearPreparseData();
  }

  // Add shared function info to new script's list. If a collection occurs,
  // the shared function info may be temporarily in two lists.
  // This is okay because the gc-time processing of these lists can tolerate
  // duplicates.
  if (script_object.IsScript()) {
    DCHECK(!script().IsScript());
    Script script = Script::cast(script_object);
    WeakFixedArray list = script.shared_function_infos();
#ifdef DEBUG
    DCHECK_LT(function_literal_id, list.length());
    MaybeObject maybe_object = list.Get(function_literal_id);
    HeapObject heap_object;
    if (maybe_object->GetHeapObjectIfWeak(&heap_object)) {
      DCHECK_EQ(heap_object, *this);
    }
#endif
    list.Set(function_literal_id, HeapObjectReference::Weak(*this));
  } else {
    DCHECK(script().IsScript());

    // Remove shared function info from old script's list.
    Script old_script = Script::cast(script());

    // Due to liveedit, it might happen that the old_script doesn't know
    // about the SharedFunctionInfo, so we have to guard against that.
    WeakFixedArray infos = old_script.shared_function_infos();
    if (function_literal_id < infos.length()) {
      MaybeObject raw =
          old_script.shared_function_infos().Get(function_literal_id);
      HeapObject heap_object;
      if (raw->GetHeapObjectIfWeak(&heap_object) && heap_object == *this) {
        old_script.shared_function_infos().Set(
            function_literal_id,
            HeapObjectReference::Strong(roots.undefined_value()));
      }
    }
  }

  // Finally set new script.
  set_script(script_object);
}

bool SharedFunctionInfo::HasBreakInfo() const {
  if (!HasDebugInfo()) return false;
  DebugInfo info = GetDebugInfo();
  bool has_break_info = info.HasBreakInfo();
  return has_break_info;
}

bool SharedFunctionInfo::BreakAtEntry() const {
  if (!HasDebugInfo()) return false;
  DebugInfo info = GetDebugInfo();
  bool break_at_entry = info.BreakAtEntry();
  return break_at_entry;
}

bool SharedFunctionInfo::HasCoverageInfo() const {
  if (!HasDebugInfo()) return false;
  DebugInfo info = GetDebugInfo();
  bool has_coverage_info = info.HasCoverageInfo();
  return has_coverage_info;
}

CoverageInfo SharedFunctionInfo::GetCoverageInfo() const {
  DCHECK(HasCoverageInfo());
  return CoverageInfo::cast(GetDebugInfo().coverage_info());
}

String SharedFunctionInfo::DebugName() {
  DisallowHeapAllocation no_gc;
  String function_name = Name();
  if (function_name.length() > 0) return function_name;
  return inferred_name();
}

bool SharedFunctionInfo::PassesFilter(const char* raw_filter) {
  Vector<const char> filter = CStrVector(raw_filter);
  std::unique_ptr<char[]> cstrname(DebugName().ToCString());
  return v8::internal::PassesFilter(CStrVector(cstrname.get()), filter);
}

bool SharedFunctionInfo::HasSourceCode() const {
  ReadOnlyRoots roots = GetReadOnlyRoots();
  return !script().IsUndefined(roots) &&
         !Script::cast(script()).source().IsUndefined(roots) &&
         String::cast(Script::cast(script()).source()).length() > 0;
}

void SharedFunctionInfo::DiscardCompiledMetadata(
    Isolate* isolate,
    std::function<void(HeapObject object, ObjectSlot slot, HeapObject target)>
        gc_notify_updated_slot) {
  DisallowHeapAllocation no_gc;
  if (is_compiled()) {
    if (FLAG_trace_flush_bytecode) {
      CodeTracer::Scope scope(GetIsolate()->GetCodeTracer());
      PrintF(scope.file(), "[discarding compiled metadata for ");
      ShortPrint(scope.file());
      PrintF(scope.file(), "]\n");
    }

    HeapObject outer_scope_info;
    if (scope_info().HasOuterScopeInfo()) {
      outer_scope_info = scope_info().OuterScopeInfo();
    } else {
      outer_scope_info = ReadOnlyRoots(isolate).the_hole_value();
    }

    // Raw setter to avoid validity checks, since we're performing the unusual
    // task of decompiling.
    set_raw_outer_scope_info_or_feedback_metadata(outer_scope_info);
    gc_notify_updated_slot(
        *this,
        RawField(SharedFunctionInfo::kOuterScopeInfoOrFeedbackMetadataOffset),
        outer_scope_info);
  } else {
    DCHECK(outer_scope_info().IsScopeInfo() || outer_scope_info().IsTheHole());
  }

  // TODO(rmcilroy): Possibly discard ScopeInfo here as well.
}

// static
void SharedFunctionInfo::DiscardCompiled(
    Isolate* isolate, Handle<SharedFunctionInfo> shared_info) {
  DCHECK(shared_info->CanDiscardCompiled());

  Handle<String> inferred_name_val =
      handle(shared_info->inferred_name(), isolate);
  int start_position = shared_info->StartPosition();
  int end_position = shared_info->EndPosition();

  shared_info->DiscardCompiledMetadata(isolate);

  // Replace compiled data with a new UncompiledData object.
  if (shared_info->HasUncompiledDataWithPreparseData()) {
    // If this is uncompiled data with a pre-parsed scope data, we can just
    // clear out the scope data and keep the uncompiled data.
    shared_info->ClearPreparseData();
  } else {
    // Create a new UncompiledData, without pre-parsed scope, and update the
    // function data to point to it. Use the raw function data setter to avoid
    // validity checks, since we're performing the unusual task of decompiling.
    Handle<UncompiledData> data =
        isolate->factory()->NewUncompiledDataWithoutPreparseData(
            inferred_name_val, start_position, end_position);
    shared_info->set_function_data(*data, kReleaseStore);
  }
}

// static
Handle<Object> SharedFunctionInfo::GetSourceCode(
    Handle<SharedFunctionInfo> shared) {
  Isolate* isolate = shared->GetIsolate();
  if (!shared->HasSourceCode()) return isolate->factory()->undefined_value();
  Handle<String> source(String::cast(Script::cast(shared->script()).source()),
                        isolate);
  return isolate->factory()->NewSubString(source, shared->StartPosition(),
                                          shared->EndPosition());
}

// static
Handle<Object> SharedFunctionInfo::GetSourceCodeHarmony(
    Handle<SharedFunctionInfo> shared) {
  Isolate* isolate = shared->GetIsolate();
  if (!shared->HasSourceCode()) return isolate->factory()->undefined_value();
  Handle<String> script_source(
      String::cast(Script::cast(shared->script()).source()), isolate);
  int start_pos = shared->function_token_position();
  DCHECK_NE(start_pos, kNoSourcePosition);
  Handle<String> source = isolate->factory()->NewSubString(
      script_source, start_pos, shared->EndPosition());
  if (!shared->is_wrapped()) return source;

  DCHECK(!shared->name_should_print_as_anonymous());
  IncrementalStringBuilder builder(isolate);
  builder.AppendCString("function ");
  builder.AppendString(Handle<String>(shared->Name(), isolate));
  builder.AppendCString("(");
  Handle<FixedArray> args(Script::cast(shared->script()).wrapped_arguments(),
                          isolate);
  int argc = args->length();
  for (int i = 0; i < argc; i++) {
    if (i > 0) builder.AppendCString(", ");
    builder.AppendString(Handle<String>(String::cast(args->get(i)), isolate));
  }
  builder.AppendCString(") {\n");
  builder.AppendString(source);
  builder.AppendCString("\n}");
  return builder.Finish().ToHandleChecked();
}

SharedFunctionInfo::Inlineability SharedFunctionInfo::GetInlineability() const {
  if (!script().IsScript()) return kHasNoScript;

  if (GetIsolate()->is_precise_binary_code_coverage() &&
      !has_reported_binary_coverage()) {
    // We may miss invocations if this function is inlined.
    return kNeedsBinaryCoverage;
  }

  if (optimization_disabled()) return kHasOptimizationDisabled;

  // Built-in functions are handled by the JSCallReducer.
  if (HasBuiltinId()) return kIsBuiltin;

  if (!IsUserJavaScript()) return kIsNotUserCode;

  // If there is no bytecode array, it is either not compiled or it is compiled
  // with WebAssembly for the asm.js pipeline. In either case we don't want to
  // inline.
  if (!HasBytecodeArray()) return kHasNoBytecode;

  if (GetBytecodeArray().length() > FLAG_max_inlined_bytecode_size) {
    return kExceedsBytecodeLimit;
  }

  if (HasBreakInfo()) return kMayContainBreakPoints;

  return kIsInlineable;
}

int SharedFunctionInfo::SourceSize() { return EndPosition() - StartPosition(); }

// Output the source code without any allocation in the heap.
std::ostream& operator<<(std::ostream& os, const SourceCodeOf& v) {
  const SharedFunctionInfo s = v.value;
  // For some native functions there is no source.
  if (!s.HasSourceCode()) return os << "<No Source>";

  // Get the source for the script which this function came from.
  // Don't use String::cast because we don't want more assertion errors while
  // we are already creating a stack dump.
  String script_source =
      String::unchecked_cast(Script::cast(s.script()).source());

  if (!script_source.LooksValid()) return os << "<Invalid Source>";

  if (!s.is_toplevel()) {
    os << "function ";
    String name = s.Name();
    if (name.length() > 0) {
      name.PrintUC16(os);
    }
  }

  int len = s.EndPosition() - s.StartPosition();
  if (len <= v.max_length || v.max_length < 0) {
    script_source.PrintUC16(os, s.StartPosition(), s.EndPosition());
    return os;
  } else {
    script_source.PrintUC16(os, s.StartPosition(),
                            s.StartPosition() + v.max_length);
    return os << "...\n";
  }
}

MaybeHandle<Code> SharedFunctionInfo::TryGetCachedCode(Isolate* isolate) {
  if (!may_have_cached_code()) return {};
  Handle<SharedFunctionInfo> zis(*this, isolate);
  return isolate->compilation_cache()->LookupCode(zis);
}

void SharedFunctionInfo::DisableOptimization(BailoutReason reason) {
  DCHECK_NE(reason, BailoutReason::kNoReason);

  set_flags(DisabledOptimizationReasonBits::update(flags(), reason));
  // Code should be the lazy compilation stub or else interpreted.
  DCHECK(abstract_code().kind() == CodeKind::INTERPRETED_FUNCTION ||
         abstract_code().kind() == CodeKind::BUILTIN);
  PROFILE(GetIsolate(),
          CodeDisableOptEvent(handle(abstract_code(), GetIsolate()),
                              handle(*this, GetIsolate())));
  if (FLAG_trace_opt) {
    CodeTracer::Scope scope(GetIsolate()->GetCodeTracer());
    PrintF(scope.file(), "[disabled optimization for ");
    ShortPrint(scope.file());
    PrintF(scope.file(), ", reason: %s]\n", GetBailoutReason(reason));
  }
}

// static
template <typename LocalIsolate>
void SharedFunctionInfo::InitFromFunctionLiteral(
    LocalIsolate* isolate, Handle<SharedFunctionInfo> shared_info,
    FunctionLiteral* lit, bool is_toplevel) {
  DCHECK(!shared_info->name_or_scope_info(kAcquireLoad).IsScopeInfo());

  // When adding fields here, make sure DeclarationScope::AnalyzePartially is
  // updated accordingly.
  shared_info->set_internal_formal_parameter_count(lit->parameter_count());
  shared_info->SetFunctionTokenPosition(lit->function_token_position(),
                                        lit->start_position());
  shared_info->set_syntax_kind(lit->syntax_kind());
  shared_info->set_allows_lazy_compilation(lit->AllowsLazyCompilation());
  shared_info->set_language_mode(lit->language_mode());
  shared_info->set_function_literal_id(lit->function_literal_id());
  // FunctionKind must have already been set.
  DCHECK(lit->kind() == shared_info->kind());
  shared_info->set_needs_home_object(lit->scope()->NeedsHomeObject());
  DCHECK_IMPLIES(lit->requires_instance_members_initializer(),
                 IsClassConstructor(lit->kind()));
  shared_info->set_requires_instance_members_initializer(
      lit->requires_instance_members_initializer());
  DCHECK_IMPLIES(lit->class_scope_has_private_brand(),
                 IsClassConstructor(lit->kind()));
  shared_info->set_class_scope_has_private_brand(
      lit->class_scope_has_private_brand());
  DCHECK_IMPLIES(lit->has_static_private_methods_or_accessors(),
                 IsClassConstructor(lit->kind()));
  shared_info->set_has_static_private_methods_or_accessors(
      lit->has_static_private_methods_or_accessors());

  shared_info->set_is_toplevel(is_toplevel);
  DCHECK(shared_info->outer_scope_info().IsTheHole());
  if (!is_toplevel) {
    Scope* outer_scope = lit->scope()->GetOuterScopeWithContext();
    if (outer_scope) {
      shared_info->set_outer_scope_info(*outer_scope->scope_info());
      shared_info->set_private_name_lookup_skips_outer_class(
          lit->scope()->private_name_lookup_skips_outer_class());
    }
  }

  shared_info->set_length(lit->function_length());

  // For lazy parsed functions, the following flags will be inaccurate since we
  // don't have the information yet. They're set later in
  // SetSharedFunctionFlagsFromLiteral (compiler.cc), when the function is
  // really parsed and compiled.
  if (lit->ShouldEagerCompile()) {
    shared_info->set_has_duplicate_parameters(lit->has_duplicate_parameters());
    shared_info->UpdateAndFinalizeExpectedNofPropertiesFromEstimate(lit);
    shared_info->set_is_safe_to_skip_arguments_adaptor(
        lit->SafeToSkipArgumentsAdaptor());
    DCHECK_NULL(lit->produced_preparse_data());

    // If we're about to eager compile, we'll have the function literal
    // available, so there's no need to wastefully allocate an uncompiled data.
    return;
  }

  shared_info->set_is_safe_to_skip_arguments_adaptor(false);
  shared_info->UpdateExpectedNofPropertiesFromEstimate(lit);

  Handle<UncompiledData> data;

  ProducedPreparseData* scope_data = lit->produced_preparse_data();
  if (scope_data != nullptr) {
    Handle<PreparseData> preparse_data = scope_data->Serialize(isolate);

    data = isolate->factory()->NewUncompiledDataWithPreparseData(
        lit->GetInferredName(isolate), lit->start_position(),
        lit->end_position(), preparse_data);
  } else {
    data = isolate->factory()->NewUncompiledDataWithoutPreparseData(
        lit->GetInferredName(isolate), lit->start_position(),
        lit->end_position());
  }

  shared_info->set_uncompiled_data(*data);
}

template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE) void SharedFunctionInfo::
    InitFromFunctionLiteral<Isolate>(Isolate* isolate,
                                     Handle<SharedFunctionInfo> shared_info,
                                     FunctionLiteral* lit, bool is_toplevel);
template EXPORT_TEMPLATE_DEFINE(V8_EXPORT_PRIVATE) void SharedFunctionInfo::
    InitFromFunctionLiteral<LocalIsolate>(
        LocalIsolate* isolate, Handle<SharedFunctionInfo> shared_info,
        FunctionLiteral* lit, bool is_toplevel);

uint16_t SharedFunctionInfo::get_property_estimate_from_literal(
    FunctionLiteral* literal) {
  int estimate = literal->expected_property_count();

  // If this is a class constructor, we may have already parsed fields.
  if (is_class_constructor()) {
    estimate += expected_nof_properties();
  }
  return estimate;
}

void SharedFunctionInfo::UpdateExpectedNofPropertiesFromEstimate(
    FunctionLiteral* literal) {
  // Limit actual estimate to fit in a 8 bit field, we will never allocate
  // more than this in any case.
  STATIC_ASSERT(JSObject::kMaxInObjectProperties <= kMaxUInt8);
  int estimate = get_property_estimate_from_literal(literal);
  set_expected_nof_properties(std::min(estimate, kMaxUInt8));
}

void SharedFunctionInfo::UpdateAndFinalizeExpectedNofPropertiesFromEstimate(
    FunctionLiteral* literal) {
  DCHECK(literal->ShouldEagerCompile());
  if (are_properties_final()) {
    return;
  }
  int estimate = get_property_estimate_from_literal(literal);

  // If no properties are added in the constructor, they are more likely
  // to be added later.
  if (estimate == 0) estimate = 2;

  // Limit actual estimate to fit in a 8 bit field, we will never allocate
  // more than this in any case.
  STATIC_ASSERT(JSObject::kMaxInObjectProperties <= kMaxUInt8);
  estimate = std::min(estimate, kMaxUInt8);

  set_expected_nof_properties(estimate);
  set_are_properties_final(true);
}

void SharedFunctionInfo::SetFunctionTokenPosition(int function_token_position,
                                                  int start_position) {
  int offset;
  if (function_token_position == kNoSourcePosition) {
    offset = 0;
  } else {
    offset = start_position - function_token_position;
  }

  if (offset > kMaximumFunctionTokenOffset) {
    offset = kFunctionTokenOutOfRange;
  }
  set_raw_function_token_offset(offset);
}

int SharedFunctionInfo::StartPosition() const {
  Object maybe_scope_info = name_or_scope_info(kAcquireLoad);
  if (maybe_scope_info.IsScopeInfo()) {
    ScopeInfo info = ScopeInfo::cast(maybe_scope_info);
    if (info.HasPositionInfo()) {
      return info.StartPosition();
    }
  }
  if (HasUncompiledData()) {
    // Works with or without scope.
    return uncompiled_data().start_position();
  }
  if (IsApiFunction() || HasBuiltinId()) {
    DCHECK_IMPLIES(HasBuiltinId(), builtin_id() != Builtins::kCompileLazy);
    return 0;
  }
  if (HasWasmExportedFunctionData()) {
    WasmInstanceObject instance = wasm_exported_function_data().instance();
    int func_index = wasm_exported_function_data().function_index();
    auto& function = instance.module()->functions[func_index];
    return static_cast<int>(function.code.offset());
  }
  return kNoSourcePosition;
}

int SharedFunctionInfo::EndPosition() const {
  Object maybe_scope_info = name_or_scope_info(kAcquireLoad);
  if (maybe_scope_info.IsScopeInfo()) {
    ScopeInfo info = ScopeInfo::cast(maybe_scope_info);
    if (info.HasPositionInfo()) {
      return info.EndPosition();
    }
  }
  if (HasUncompiledData()) {
    // Works with or without scope.
    return uncompiled_data().end_position();
  }
  if (IsApiFunction() || HasBuiltinId()) {
    DCHECK_IMPLIES(HasBuiltinId(), builtin_id() != Builtins::kCompileLazy);
    return 0;
  }
  if (HasWasmExportedFunctionData()) {
    WasmInstanceObject instance = wasm_exported_function_data().instance();
    int func_index = wasm_exported_function_data().function_index();
    auto& function = instance.module()->functions[func_index];
    return static_cast<int>(function.code.end_offset());
  }
  return kNoSourcePosition;
}

void SharedFunctionInfo::SetPosition(int start_position, int end_position) {
  Object maybe_scope_info = name_or_scope_info(kAcquireLoad);
  if (maybe_scope_info.IsScopeInfo()) {
    ScopeInfo info = ScopeInfo::cast(maybe_scope_info);
    if (info.HasPositionInfo()) {
      info.SetPositionInfo(start_position, end_position);
    }
  } else if (HasUncompiledData()) {
    if (HasUncompiledDataWithPreparseData()) {
      // Clear out preparsed scope data, since the position setter invalidates
      // any scope data.
      ClearPreparseData();
    }
    uncompiled_data().set_start_position(start_position);
    uncompiled_data().set_end_position(end_position);
  } else {
    UNREACHABLE();
  }
}

bool SharedFunctionInfo::AreSourcePositionsAvailable() const {
  if (FLAG_enable_lazy_source_positions) {
    return !HasBytecodeArray() || GetBytecodeArray().HasSourcePositionTable();
  }
  return true;
}

// static
void SharedFunctionInfo::EnsureSourcePositionsAvailable(
    Isolate* isolate, Handle<SharedFunctionInfo> shared_info) {
  if (FLAG_enable_lazy_source_positions && shared_info->HasBytecodeArray() &&
      !shared_info->GetBytecodeArray().HasSourcePositionTable()) {
    Compiler::CollectSourcePositions(isolate, shared_info);
  }
}

}  // namespace internal
}  // namespace v8
