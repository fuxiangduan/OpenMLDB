/*-------------------------------------------------------------------------
 * Copyright (C) 2019, 4paradigm
 * udf_registry.h
 *
 * Author: chenjing
 * Date: 2019/11/26
 *--------------------------------------------------------------------------
 **/
#ifndef SRC_UDF_UDF_REGISTRY_H_
#define SRC_UDF_UDF_REGISTRY_H_

#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/fe_status.h"
#include "codec/list_iterator_codec.h"
#include "codegen/context.h"
#include "node/node_manager.h"
#include "node/sql_node.h"
#include "udf/literal_traits.h"
#include "udf/udf_library.h"
#include "vm/schemas_context.h"

namespace fesql {
namespace udf {

using fesql::base::Status;
using fesql::codec::StringRef;
using fesql::node::ExprListNode;
using fesql::node::ExprNode;
using fesql::node::SQLNode;

/**
 * Overall information to resolve a sql function call.
 */
class UDFResolveContext {
 public:
    UDFResolveContext(const std::vector<node::ExprNode*>& args,
                      node::NodeManager* node_manager,
                      const udf::UDFLibrary* library)
        : args_(args), node_manager_(node_manager), library_(library) {}

    const std::vector<node::ExprNode*>& args() const { return args_; }
    node::NodeManager* node_manager() { return node_manager_; }
    const udf::UDFLibrary* library() { return library_; }

    size_t arg_size() const { return args_.size(); }
    const node::TypeNode* arg_type(size_t i) const {
        return args_[i]->GetOutputType();
    }
    bool arg_nullable(size_t i) const { return args_[i]->nullable(); }

    const std::string GetArgSignature() const;

    const std::string& GetError() const { return error_msg_; }
    void SetError(const std::string& err) { error_msg_ = err; }
    bool HasError() const { return error_msg_ != ""; }

 private:
    std::vector<node::ExprNode*> args_;
    node::NodeManager* node_manager_;
    const udf::UDFLibrary* library_;

    std::string error_msg_;
};

/**
 * Interface to implement resolve logic for sql function
 * call without extra transformation.
 */
class UDFRegistry {
 public:
    explicit UDFRegistry(const std::string& name) : name_(name) {}

    virtual ~UDFRegistry() {}

    virtual Status Transform(UDFResolveContext* ctx, node::ExprNode** result) {
        node::FnDefNode* fn_def = nullptr;
        CHECK_STATUS(ResolveFunction(ctx, &fn_def));
        *result =
            ctx->node_manager()->MakeFuncNode(fn_def, ctx->args(), nullptr);
        return Status::OK();
    }

    // "f(arg0, arg1, ...argN)" -> resolved f
    virtual Status ResolveFunction(UDFResolveContext* ctx,
                                   node::FnDefNode** result) = 0;

    const std::string& name() const { return name_; }

    void SetDoc(const std::string& doc) { this->doc_ = doc; }

    const std::string& doc() const { return doc_; }

 private:
    std::string name_;
    std::string doc_;
};

template <typename T>
class ArgSignatureTable {
 public:
    Status Find(UDFResolveContext* ctx, T* res, std::string* signature,
                int* variadic_pos) {
        std::vector<const node::TypeNode*> arg_types;
        for (size_t i = 0; i < ctx->arg_size(); ++i) {
            arg_types.push_back(ctx->arg_type(i));
        }
        return Find(arg_types, res, signature, variadic_pos);
    }

    Status Find(const std::vector<const node::TypeNode*>& arg_types, T* res,
                std::string* signature, int* variadic_pos) {
        std::stringstream ss;
        for (size_t i = 0; i < arg_types.size(); ++i) {
            auto type_node = arg_types[i];
            if (type_node == nullptr) {
                ss << "?";
            } else {
                ss << type_node->GetName();
            }
            if (i < arg_types.size() - 1) {
                ss << ", ";
            }
        }

        // There are four match conditions:
        // (1) explicit match without placeholders
        // (2) explicit match with placeholders
        // (3) variadic match without placeholders
        // (4) variadic match with placeholders
        // The priority is (1) > (2) > (3) > (4)
        typename TableType::iterator placeholder_match_iter = table_.end();
        typename TableType::iterator variadic_placeholder_match_iter =
            table_.end();
        typename TableType::iterator variadic_match_iter = table_.end();
        int variadic_match_pos = -1;
        int variadic_placeholder_match_pos = -1;

        for (auto iter = table_.begin(); iter != table_.end(); ++iter) {
            auto& def_item = iter->second;
            auto& def_arg_types = def_item.arg_types;
            if (def_item.is_variadic) {
                // variadic match
                bool match = true;
                bool placeholder_match = false;
                int non_variadic_arg_num = def_arg_types.size();
                if (arg_types.size() <
                    static_cast<size_t>(non_variadic_arg_num)) {
                    continue;
                }
                for (int j = 0; j < non_variadic_arg_num; ++j) {
                    if (def_arg_types[j] == nullptr) {  // any arg
                        placeholder_match = true;
                        match = false;
                    } else if (!node::TypeEquals(def_arg_types[j],
                                                 arg_types[j])) {
                        placeholder_match = false;
                        match = false;
                        break;
                    }
                }
                if (match) {
                    if (variadic_match_pos < non_variadic_arg_num) {
                        placeholder_match_iter = iter;
                        variadic_match_pos = non_variadic_arg_num;
                    }
                } else if (placeholder_match) {
                    if (variadic_placeholder_match_pos < non_variadic_arg_num) {
                        variadic_placeholder_match_iter = iter;
                        variadic_placeholder_match_pos = non_variadic_arg_num;
                    }
                }

            } else if (arg_types.size() == def_arg_types.size()) {
                // explicit match
                bool match = true;
                bool placeholder_match = false;
                for (size_t j = 0; j < arg_types.size(); ++j) {
                    if (def_arg_types[j] == nullptr) {
                        placeholder_match = true;
                        match = false;
                    } else if (!node::TypeEquals(def_arg_types[j],
                                                 arg_types[j])) {
                        placeholder_match = false;
                        match = false;
                        break;
                    }
                }
                if (match) {
                    *variadic_pos = -1;
                    *signature = iter->first;
                    *res = def_item.value;
                    return Status::OK();
                } else if (placeholder_match) {
                    placeholder_match_iter = iter;
                }
            }
        }

        if (placeholder_match_iter != table_.end()) {
            *variadic_pos = -1;
            *signature = placeholder_match_iter->first;
            *res = placeholder_match_iter->second.value;
            return Status::OK();
        } else if (variadic_match_iter != table_.end()) {
            *variadic_pos = variadic_match_pos;
            *signature = variadic_match_iter->first;
            *res = variadic_match_iter->second.value;
            return Status::OK();
        } else if (variadic_placeholder_match_iter != table_.end()) {
            *variadic_pos = variadic_placeholder_match_pos;
            *signature = variadic_placeholder_match_iter->first;
            *res = variadic_placeholder_match_iter->second.value;
            return Status::OK();
        } else {
            return Status(common::kCodegenError,
                          "Resolve udf signature failure: <" + ss.str() + ">");
        }
    }

    Status Register(const std::vector<const node::TypeNode*>& args,
                    bool is_variadic, const T& t) {
        std::stringstream ss;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == nullptr) {
                ss << "?";
            } else {
                ss << args[i]->GetName();
            }
            if (i < args.size() - 1) {
                ss << ", ";
            }
        }
        std::string key = ss.str();
        auto iter = table_.find(key);
        CHECK_TRUE(iter == table_.end(), "Duplicate signature: ", key);
        table_.insert(iter, {key, DefItem(t, args, is_variadic)});
        return Status::OK();
    }

    struct DefItem {
        T value;
        std::vector<const node::TypeNode*> arg_types;
        bool is_variadic;
        DefItem(const T& value,
                const std::vector<const node::TypeNode*>& arg_types,
                bool is_variadic)
            : value(value), arg_types(arg_types), is_variadic(is_variadic) {}
    };

    using TableType = std::unordered_map<std::string, DefItem>;

    const TableType& GetTable() const { return table_; }

 private:
    TableType table_;
};

struct ExprUDFGenBase {
    virtual ExprNode* gen(UDFResolveContext* ctx,
                          const std::vector<ExprNode*>& args) = 0;
    virtual ~ExprUDFGenBase() {}
};

template <typename... Args>
struct ExprUDFGen : public ExprUDFGenBase {
    using FType = std::function<ExprNode*(
        UDFResolveContext*,
        typename std::pair<Args, ExprNode*>::second_type...)>;

    ExprNode* gen(UDFResolveContext* ctx,
                  const std::vector<ExprNode*>& args) override {
        if (args.size() != sizeof...(Args)) {
            LOG(WARNING) << "Fail to invoke ExprUDFGen::gen, args size do not "
                            "match with template args)";
            return nullptr;
        }
        return gen_internal(ctx, args, std::index_sequence_for<Args...>());
    }

    template <std::size_t... I>
    ExprNode* gen_internal(UDFResolveContext* ctx,
                           const std::vector<ExprNode*>& args,
                           const std::index_sequence<I...>&) {
        return gen_func(ctx, args[I]...);
    }

    explicit ExprUDFGen(const FType& f) : gen_func(f) {}
    const FType gen_func;
};

template <typename... Args>
struct VariadicExprUDFGen : public ExprUDFGenBase {
    using FType = std::function<ExprNode*(
        UDFResolveContext*, typename std::pair<Args, ExprNode*>::second_type...,
        const std::vector<ExprNode*>&)>;

    ExprNode* gen(UDFResolveContext* ctx,
                  const std::vector<ExprNode*>& args) override {
        return gen_internal(ctx, args, std::index_sequence_for<Args...>());
    };

    template <std::size_t... I>
    ExprNode* gen_internal(UDFResolveContext* ctx,
                           const std::vector<ExprNode*>& args,
                           const std::index_sequence<I...>&) {
        if (args.size() < sizeof...(Args)) {
            LOG(WARNING) << "Fail to invoke VariadicExprUDFGen::gen, "
                            "args size do not match with template args)";
            return nullptr;
        }
        std::vector<ExprNode*> variadic_args;
        for (size_t i = sizeof...(I); i < args.size(); ++i) {
            variadic_args.emplace_back(args[i]);
        }
        return this->gen_func(ctx, args[I]..., variadic_args);
    }

    explicit VariadicExprUDFGen(const FType& f) : gen_func(f) {}
    const FType gen_func;
};

template <typename RegistryT>
class UDFRegistryHelper {
 public:
    explicit UDFRegistryHelper(const std::string& name, UDFLibrary* library)
        : name_(name), library_(library) {}

    UDFLibrary* library() const { return library_; }

    node::NodeManager* node_manager() const { return library_->node_manager(); }

    const std::string& name() const { return name_; }
    const std::string& doc() const { return doc_; }

    const std::string& GetDoc() const { return doc(); }

    void SetDoc(const std::string& doc) {
        doc_ = doc;
        for (auto reg : registries_) {
            reg->SetDoc(doc);
        }
    }

    void InsertRegistry(const std::vector<const node::TypeNode*>& signature,
                        bool is_variadic, std::shared_ptr<RegistryT> registry) {
        registry->SetDoc(doc_);
        library_->InsertRegistry(name_, signature, is_variadic, registry);
        registries_.push_back(registry);
    }

 private:
    std::string name_;
    UDFLibrary* library_;
    std::string doc_;
    std::vector<std::shared_ptr<RegistryT>> registries_;
};

/**
 * Interface to resolve udf with expression construction.
 */
class ExprUDFRegistry : public UDFRegistry {
 public:
    explicit ExprUDFRegistry(const std::string& name,
                             std::shared_ptr<ExprUDFGenBase> gen_impl_func)
        : UDFRegistry(name), gen_impl_func_(gen_impl_func) {}

    Status ResolveFunction(UDFResolveContext* ctx,
                           node::FnDefNode** result) override;

 private:
    std::shared_ptr<ExprUDFGenBase> gen_impl_func_;
};

class ExprUDFRegistryHelper : public UDFRegistryHelper<ExprUDFRegistry> {
 public:
    explicit ExprUDFRegistryHelper(const std::string& name, UDFLibrary* library)
        : UDFRegistryHelper<ExprUDFRegistry>(name, library) {}

    template <typename... Args>
    ExprUDFRegistryHelper& args(
        const typename ExprUDFGen<Args...>::FType& func) {
        auto gen_ptr = std::make_shared<ExprUDFGen<Args...>>(func);
        auto registry = std::make_shared<ExprUDFRegistry>(name(), gen_ptr);
        this->InsertRegistry(
            {DataTypeTrait<Args>::to_type_node(node_manager())...}, false,
            registry);
        return *this;
    }

    template <typename... Args>
    ExprUDFRegistryHelper& variadic_args(
        const typename VariadicExprUDFGen<Args...>::FType& func) {
        auto gen_ptr = std::make_shared<VariadicExprUDFGen<Args...>>(func);
        auto registry = std::make_shared<ExprUDFRegistry>(name(), gen_ptr);
        this->InsertRegistry(
            {DataTypeTrait<Args>::to_type_node(node_manager())...}, true,
            registry);
        return *this;
    }

    ExprUDFRegistryHelper& doc(const std::string& doc) {
        SetDoc(doc);
        return *this;
    }
};

template <template <typename> typename FTemplate>
class ExprUDFTemplateRegistryHelper {
 public:
    ExprUDFTemplateRegistryHelper(const std::string& name, UDFLibrary* library)
        : helper_(library->RegisterExprUDF(name)) {}

    template <typename... Args>
    std::initializer_list<int> args_in() {
        return {
            RegisterSingle<Args, typename FTemplate<Args>::Args>()(helper_)...};
    }

    auto& doc(const std::string& str) {
        helper_.doc(str);
        return *this;
    }

 private:
    template <typename T, typename... Args>
    struct FTemplateInst {
        static ExprNode* fcompute(
            UDFResolveContext* ctx,
            typename std::pair<Args, ExprNode*>::second_type... args) {
            return FTemplate<T>()(ctx, args...);
        }
    };

    template <typename T, typename X>
    struct RegisterSingle;

    template <typename T, typename... Args>
    struct RegisterSingle<T, std::tuple<Args...>> {
        int operator()(ExprUDFRegistryHelper& helper) {  // NOLINT
            helper.args<Args...>(FTemplateInst<T, Args...>::fcompute);
            return 0;
        }
    };

    ExprUDFRegistryHelper helper_;
};

/**
 * Summarize runtime attribute of expression
 */
class ExprAttrNode {
 public:
    ExprAttrNode(const node::TypeNode* dtype, bool nullable)
        : type_(dtype), nullable_(nullable) {}

    const node::TypeNode* type() const { return type_; }
    bool nullable() const { return nullable_; }

    void SetType(const node::TypeNode* dtype) { type_ = dtype; }
    void SetNullable(bool flag) { nullable_ = flag; }

 private:
    const node::TypeNode* type_;
    bool nullable_;
};

class LLVMUDFGenBase {
 public:
    virtual Status gen(codegen::CodeGenContext* ctx,
                       const std::vector<codegen::NativeValue>& args,
                       codegen::NativeValue* res) = 0;

    virtual Status infer(UDFResolveContext* ctx,
                         const std::vector<const ExprAttrNode*>& args,
                         ExprAttrNode*) = 0;

    node::TypeNode* fixed_ret_type() const { return fixed_ret_type_; }

    void SetFixedReturnType(node::TypeNode* dtype) {
        this->fixed_ret_type_ = dtype;
    }

    virtual ~LLVMUDFGenBase() {}

 private:
    node::TypeNode* fixed_ret_type_ = nullptr;
};

template <typename... Args>
struct LLVMUDFGen : public LLVMUDFGenBase {
    using FType = std::function<Status(
        codegen::CodeGenContext* ctx,
        typename std::pair<Args, codegen::NativeValue>::second_type...,
        codegen::NativeValue*)>;

    using InferFType = std::function<Status(
        UDFResolveContext*,
        typename std::pair<Args, const ExprAttrNode*>::second_type...,
        ExprAttrNode*)>;

    Status gen(codegen::CodeGenContext* ctx,
               const std::vector<codegen::NativeValue>& args,
               codegen::NativeValue* result) override {
        CHECK_TRUE(args.size() == sizeof...(Args),
                   "Fail to invoke LLVMUDFGen::gen, args size do not "
                   "match with template args)");
        return gen_internal(ctx, args, result,
                            std::index_sequence_for<Args...>());
    }

    template <std::size_t... I>
    Status gen_internal(codegen::CodeGenContext* ctx,
                        const std::vector<codegen::NativeValue>& args,
                        codegen::NativeValue* result,
                        const std::index_sequence<I...>&) {
        return gen_func(ctx, args[I]..., result);
    }

    Status infer(UDFResolveContext* ctx,
                 const std::vector<const ExprAttrNode*>& args,
                 ExprAttrNode* out) override {
        return infer_internal(ctx, args, out,
                              std::index_sequence_for<Args...>());
    }

    template <std::size_t... I>
    Status infer_internal(UDFResolveContext* ctx,
                          const std::vector<const ExprAttrNode*>& args,
                          ExprAttrNode* out, const std::index_sequence<I...>&) {
        if (this->infer_func) {
            return infer_func(ctx, args[I]..., out);
        } else {
            out->SetType(fixed_ret_type());
            out->SetNullable(false);
            return Status::OK();
        }
    }

    LLVMUDFGen(const FType& f, const InferFType& infer)
        : gen_func(f), infer_func(infer) {}

    explicit LLVMUDFGen(const FType& f) : gen_func(f), infer_func() {}

    virtual ~LLVMUDFGen() {}
    const FType gen_func;
    const InferFType infer_func;
};

template <typename... Args>
struct VariadicLLVMUDFGen : public LLVMUDFGenBase {
    using FType = std::function<Status(
        codegen::CodeGenContext*,
        typename std::pair<Args, codegen::NativeValue>::second_type...,
        const std::vector<codegen::NativeValue>&, codegen::NativeValue*)>;

    using InferFType = std::function<Status(
        UDFResolveContext*,
        typename std::pair<Args, const ExprAttrNode*>::second_type...,
        const std::vector<const ExprAttrNode*>&, ExprAttrNode*)>;

    Status gen(codegen::CodeGenContext* ctx,
               const std::vector<codegen::NativeValue>& args,
               codegen::NativeValue* result) override {
        CHECK_TRUE(args.size() >= sizeof...(Args),
                   "Fail to invoke VariadicLLVMUDFGen::gen, "
                   "args size do not match with template args)");
        return gen_internal(ctx, args, result,
                            std::index_sequence_for<Args...>());
    };

    template <std::size_t... I>
    Status gen_internal(codegen::CodeGenContext* ctx,
                        const std::vector<codegen::NativeValue>& args,
                        codegen::NativeValue* result,
                        const std::index_sequence<I...>&) {
        std::vector<codegen::NativeValue> variadic_args;
        for (size_t i = sizeof...(I); i < args.size(); ++i) {
            variadic_args.emplace_back(args[i]);
        }
        return this->gen_func(ctx, args[I]..., variadic_args, result);
    }

    Status infer(UDFResolveContext* ctx,
                 const std::vector<const ExprAttrNode*>& args,
                 ExprAttrNode* out) override {
        return infer_internal(ctx, args, out,
                              std::index_sequence_for<Args...>());
    }

    template <std::size_t... I>
    Status infer_internal(UDFResolveContext* ctx,
                          const std::vector<const ExprAttrNode*>& args,
                          ExprAttrNode* out, const std::index_sequence<I...>&) {
        std::vector<const ExprAttrNode*> variadic_args;
        for (size_t i = sizeof...(I); i < args.size(); ++i) {
            variadic_args.emplace_back(args[i]);
        }
        if (this->infer_func) {
            return this->infer_func(ctx, args[I]..., variadic_args, out);
        } else {
            out->SetType(fixed_ret_type());
            out->SetNullable(false);
            return Status::OK();
        }
    }

    VariadicLLVMUDFGen(const FType& f, const InferFType& infer)
        : gen_func(f), infer_func(infer) {}

    explicit VariadicLLVMUDFGen(const FType& f) : gen_func(f), infer_func() {}

    const FType gen_func;
    const InferFType infer_func;
};

/**
 * Interface to resolve udf with llvm codegen construction.
 */
class LLVMUDFRegistry : public UDFRegistry {
 public:
    explicit LLVMUDFRegistry(const std::string& name,
                             std::shared_ptr<LLVMUDFGenBase> gen_impl_func,
                             const std::vector<size_t>& nullable_arg_indices)
        : UDFRegistry(name),
          gen_impl_func_(gen_impl_func),
          nullable_arg_indices_(nullable_arg_indices) {}

    Status ResolveFunction(UDFResolveContext* ctx,
                           node::FnDefNode** result) override;

 private:
    std::shared_ptr<LLVMUDFGenBase> gen_impl_func_;
    std::vector<size_t> nullable_arg_indices_;
};

class LLVMUDFRegistryHelper : public UDFRegistryHelper<LLVMUDFRegistry> {
 public:
    LLVMUDFRegistryHelper(const std::string& name, UDFLibrary* library)
        : UDFRegistryHelper<LLVMUDFRegistry>(name, library) {}

    LLVMUDFRegistryHelper(const LLVMUDFRegistryHelper& other)
        : UDFRegistryHelper<LLVMUDFRegistry>(other.name(), other.library()) {}

    template <typename RetType>
    LLVMUDFRegistryHelper& returns() {
        fixed_ret_type_ =
            DataTypeTrait<RetType>::to_type_node(library()->node_manager());
        if (cur_def_ != nullptr) {
            cur_def_->SetFixedReturnType(fixed_ret_type_);
        }
        return *this;
    }

    template <typename... Args>
    LLVMUDFRegistryHelper& args(
        const typename LLVMUDFGen<Args...>::FType& gen) {
        using InferF = typename LLVMUDFGen<Args...>::InferFType;
        return args<Args...>(InferF(), gen);
    }

    template <typename... Args>
    LLVMUDFRegistryHelper& args(
        const typename LLVMUDFGen<Args...>::InferFType& infer,
        const typename LLVMUDFGen<Args...>::FType& gen) {
        // find nullable arg positions
        std::vector<size_t> null_indices;
        std::vector<int> arg_nullable = {IsNullableTrait<Args>::value...};
        for (size_t i = 0; i < arg_nullable.size(); ++i) {
            if (arg_nullable[i] > 0) {
                null_indices.push_back(i);
            }
        }
        cur_def_ = std::make_shared<LLVMUDFGen<Args...>>(gen, infer);
        auto registry =
            std::make_shared<LLVMUDFRegistry>(name(), cur_def_, null_indices);
        this->InsertRegistry(
            {DataTypeTrait<Args>::to_type_node(node_manager())...}, false,
            registry);
        if (fixed_ret_type_ != nullptr) {
            cur_def_->SetFixedReturnType(fixed_ret_type_);
        }
        return *this;
    }

    template <typename... Args>
    LLVMUDFRegistryHelper& variadic_args(
        const typename VariadicLLVMUDFGen<Args...>::FType& gen) {  // NOLINT
        return variadic_args<Args...>([](...) { return nullptr; }, gen);
    }

    template <typename... Args>
    LLVMUDFRegistryHelper& variadic_args(
        const typename VariadicLLVMUDFGen<Args...>::InferFType& infer,
        const typename VariadicLLVMUDFGen<Args...>::FType& gen) {  // NOLINT
        // find nullable arg positions
        std::vector<size_t> null_indices;
        std::vector<int> arg_nullable = {IsNullableTrait<Args>::value...};
        for (size_t i = 0; i < arg_nullable.size(); ++i) {
            if (arg_nullable[i] > 0) {
                null_indices.push_back(i);
            }
        }
        cur_def_ = std::make_shared<VariadicLLVMUDFGen<Args...>>(gen, infer);
        auto registry =
            std::make_shared<LLVMUDFRegistry>(name(), cur_def_, null_indices);
        this->InsertRegistry(
            {DataTypeTrait<Args>::to_type_node(node_manager())...}, true,
            registry);
        if (fixed_ret_type_ != nullptr) {
            cur_def_->SetFixedReturnType(fixed_ret_type_);
        }
        return *this;
    }

    LLVMUDFRegistryHelper& doc(const std::string& doc) {
        SetDoc(doc);
        return *this;
    }

    std::shared_ptr<LLVMUDFGenBase> cur_def() const { return cur_def_; }

 private:
    std::shared_ptr<LLVMUDFGenBase> cur_def_ = nullptr;
    node::TypeNode* fixed_ret_type_ = nullptr;
};

template <template <typename> typename FTemplate>
class CodeGenUDFTemplateRegistryHelper {
 public:
    CodeGenUDFTemplateRegistryHelper(const std::string& name,
                                     UDFLibrary* library)
        : helper_(library->RegisterCodeGenUDF(name)) {}

    template <typename... Args>
    CodeGenUDFTemplateRegistryHelper& args_in() {
        cur_defs_ = {
            RegisterSingle<Args, typename FTemplate<Args>::Args>()(helper_)...};
        if (fixed_ret_type_ != nullptr) {
            for (auto def : cur_defs_) {
                def->SetFixedReturnType(fixed_ret_type_);
            }
        }
        return *this;
    }

    template <typename RetType>
    CodeGenUDFTemplateRegistryHelper& returns() {
        fixed_ret_type_ =
            DataTypeTrait<RetType>::to_type_node(helper_.node_manager());
        for (auto def : cur_defs_) {
            def->SetFixedReturnType(fixed_ret_type_);
        }
        return *this;
    }

    auto& doc(const std::string& str) {
        helper_.doc(str);
        return *this;
    }

 private:
    template <typename T, typename X>
    struct RegisterSingle;

    template <typename T, typename... Args>
    struct RegisterSingle<T, std::tuple<Args...>> {
        std::shared_ptr<LLVMUDFGenBase> operator()(
            LLVMUDFRegistryHelper& helper) {  // NOLINT
            helper.args<Args...>(
                [](codegen::CodeGenContext* ctx,
                   typename std::pair<
                       Args, codegen::NativeValue>::second_type... args,
                   codegen::NativeValue* result) {
                    return FTemplate<T>()(ctx, args..., result);
                });
            return helper.cur_def();
        }
    };

    LLVMUDFRegistryHelper helper_;
    std::vector<std::shared_ptr<LLVMUDFGenBase>> cur_defs_;
    node::TypeNode* fixed_ret_type_;
};

/**
 * Interface to resolve udf to external native functions.
 */
class ExternalFuncRegistry : public UDFRegistry {
 public:
    explicit ExternalFuncRegistry(const std::string& name,
                                  node::ExternalFnDefNode* extern_def)
        : UDFRegistry(name), extern_def_(extern_def) {}

    Status ResolveFunction(UDFResolveContext* ctx,
                           node::FnDefNode** result) override;

 private:
    node::ExternalFnDefNode* extern_def_;
};

template <bool A, bool B>
struct ConditionAnd {
    static const bool value = false;
};
template <>
struct ConditionAnd<true, true> {
    static const bool value = true;
};

template <typename Ret, typename Args, typename CRet, typename CArgs>
struct FuncTypeCheckHelper {
    static const bool value = false;
};

template <typename Arg, typename CArg>
struct FuncArgTypeCheckHelper {
    static const bool value =
        std::is_same<Arg, typename CCallDataTypeTrait<CArg>::LiteralTag>::value;
};

template <typename Ret, typename>
struct FuncRetTypeCheckHelper {
    static const bool value = false;
};
template <typename Ret>
struct FuncRetTypeCheckHelper<Ret, std::tuple<Ret*>> {
    static const bool value = true;
};
template <typename Ret>
struct FuncRetTypeCheckHelper<Nullable<Ret>, std::tuple<Ret*, bool*>> {
    static const bool value = true;
};
template <typename Ret>
struct FuncRetTypeCheckHelper<Opaque<Ret>, std::tuple<Ret*>> {
    static const bool value = true;
};

template <typename, typename>
struct FuncTupleRetTypeCheckHelper {
    using Remain = void;
    static const bool value = false;
};

template <typename TupleHead, typename... TupleTail, typename CArgHead,
          typename... CArgTail>
struct FuncTupleRetTypeCheckHelper<std::tuple<TupleHead, TupleTail...>,
                                   std::tuple<CArgHead, CArgTail...>> {
    using HeadCheck = FuncRetTypeCheckHelper<TupleHead, std::tuple<CArgHead>>;
    using TailCheck = FuncTupleRetTypeCheckHelper<std::tuple<TupleTail...>,
                                                  std::tuple<CArgTail...>>;
    using Remain = typename TailCheck::Remain;
    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

template <typename TupleHead, typename... TupleTail, typename CArgHead,
          typename... CArgTail>
struct FuncTupleRetTypeCheckHelper<
    std::tuple<Nullable<TupleHead>, TupleTail...>,
    std::tuple<CArgHead, bool*, CArgTail...>> {
    using HeadCheck = FuncRetTypeCheckHelper<TupleHead, std::tuple<CArgHead>>;
    using TailCheck = FuncTupleRetTypeCheckHelper<std::tuple<TupleTail...>,
                                                  std::tuple<CArgTail...>>;
    using Remain = typename TailCheck::Remain;
    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

template <typename... TupleArgs, typename... TupleTail, typename CArgHead,
          typename... CArgTail>
struct FuncTupleRetTypeCheckHelper<
    std::tuple<Tuple<TupleArgs...>, TupleTail...>,
    std::tuple<CArgHead, CArgTail...>> {
    using RecCheck =
        FuncTupleRetTypeCheckHelper<std::tuple<TupleArgs...>,
                                    std::tuple<CArgHead, CArgTail...>>;
    using RecRemain = typename RecCheck::Remain;
    using TailCheck =
        FuncTupleRetTypeCheckHelper<std::tuple<TupleTail...>, RecRemain>;
    using Remain = typename TailCheck::Remain;
    static const bool value =
        ConditionAnd<RecCheck::value, TailCheck::value>::value;
};

template <typename... CArgs>
struct FuncTupleRetTypeCheckHelper<std::tuple<>, std::tuple<CArgs...>> {
    static const bool value = true;
    using Remain = std::tuple<CArgs...>;
};

template <typename... TupleArgs, typename... CArgs>
struct FuncRetTypeCheckHelper<Tuple<TupleArgs...>, std::tuple<CArgs...>> {
    using RecCheck = FuncTupleRetTypeCheckHelper<std::tuple<TupleArgs...>,
                                                 std::tuple<CArgs...>>;
    static const bool value =
        ConditionAnd<RecCheck::value, std::is_same<typename RecCheck::Remain,
                                                   std::tuple<>>::value>::value;
};

template <typename Ret, typename ArgHead, typename... ArgTail, typename CRet,
          typename CArgHead, typename... CArgTail>
struct FuncTypeCheckHelper<Ret, std::tuple<ArgHead, ArgTail...>, CRet,
                           std::tuple<CArgHead, CArgTail...>> {
    using HeadCheck = FuncArgTypeCheckHelper<ArgHead, CArgHead>;

    using TailCheck = FuncTypeCheckHelper<Ret, std::tuple<ArgTail...>, CRet,
                                          std::tuple<CArgTail...>>;

    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

template <typename Ret, typename ArgHead, typename... ArgTail, typename CRet,
          typename CArgHead, typename... CArgTail>
struct FuncTypeCheckHelper<Ret, std::tuple<Nullable<ArgHead>, ArgTail...>, CRet,
                           std::tuple<CArgHead, bool, CArgTail...>> {
    using HeadCheck = FuncArgTypeCheckHelper<ArgHead, CArgHead>;

    using TailCheck = FuncTypeCheckHelper<Ret, std::tuple<ArgTail...>, CRet,
                                          std::tuple<CArgTail...>>;

    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

template <typename, typename>
struct FuncTupleArgTypeCheckHelper {
    using Remain = void;
    static const bool value = false;
};

template <typename TupleHead, typename... TupleTail, typename CArgHead,
          typename... CArgTail>
struct FuncTupleArgTypeCheckHelper<std::tuple<TupleHead, TupleTail...>,
                                   std::tuple<CArgHead, CArgTail...>> {
    using HeadCheck = FuncArgTypeCheckHelper<TupleHead, CArgHead>;
    using TailCheck = FuncTupleArgTypeCheckHelper<std::tuple<TupleTail...>,
                                                  std::tuple<CArgTail...>>;
    using Remain = typename TailCheck::Remain;
    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

template <typename TupleHead, typename... TupleTail, typename CArgHead,
          typename... CArgTail>
struct FuncTupleArgTypeCheckHelper<
    std::tuple<Nullable<TupleHead>, TupleTail...>,
    std::tuple<CArgHead, bool, CArgTail...>> {
    using HeadCheck = FuncArgTypeCheckHelper<TupleHead, CArgHead>;
    using TailCheck = FuncTupleArgTypeCheckHelper<std::tuple<TupleTail...>,
                                                  std::tuple<CArgTail...>>;
    using Remain = typename TailCheck::Remain;
    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

template <typename... TupleArgs, typename... TupleTail, typename CArgHead,
          typename... CArgTail>
struct FuncTupleArgTypeCheckHelper<
    std::tuple<Tuple<TupleArgs...>, TupleTail...>,
    std::tuple<CArgHead, CArgTail...>> {
    using RecCheck =
        FuncTupleArgTypeCheckHelper<std::tuple<TupleArgs...>,
                                    std::tuple<CArgHead, CArgTail...>>;
    using RecRemain = typename RecCheck::Remain;
    using TailCheck =
        FuncTupleArgTypeCheckHelper<std::tuple<TupleTail...>, RecRemain>;
    using Remain = typename TailCheck::Remain;
    static const bool value =
        ConditionAnd<RecCheck::value, TailCheck::value>::value;
};

template <typename... CArgs>
struct FuncTupleArgTypeCheckHelper<std::tuple<>, std::tuple<CArgs...>> {
    static const bool value = true;
    using Remain = std::tuple<CArgs...>;
};

template <typename Ret, typename... TupleArgs, typename... ArgTail,
          typename CRet, typename CArgHead, typename... CArgTail>
struct FuncTypeCheckHelper<Ret, std::tuple<Tuple<TupleArgs...>, ArgTail...>,
                           CRet, std::tuple<CArgHead, CArgTail...>> {
    using HeadCheck =
        FuncTupleArgTypeCheckHelper<std::tuple<TupleArgs...>,
                                    std::tuple<CArgHead, CArgTail...>>;

    using TailCheck = FuncTypeCheckHelper<Ret, std::tuple<ArgTail...>, CRet,
                                          typename HeadCheck::Remain>;

    static const bool value =
        ConditionAnd<HeadCheck::value, TailCheck::value>::value;
};

// All input arg check passed, check return by arg convention
template <typename Ret, typename CArgHead, typename... CArgTail>
struct FuncTypeCheckHelper<Ret, std::tuple<>, void,
                           std::tuple<CArgHead, CArgTail...>> {
    static const bool value =
        FuncRetTypeCheckHelper<Ret, std::tuple<CArgHead, CArgTail...>>::value;
};

// All input arg check passed, check simple return
template <typename Ret, typename CRet>
struct FuncTypeCheckHelper<Ret, std::tuple<>, CRet, std::tuple<>> {
    static const bool value = FuncArgTypeCheckHelper<Ret, CRet>::value;
};

template <typename>
struct TypeAnnotatedFuncPtrImpl;  // primitive decl

template <typename Ret, typename EnvArgs, typename CRet, typename CArgs>
struct StaticFuncTypeCheck {
    static void check() {
        using Checker = FuncTypeCheckHelper<Ret, EnvArgs, CRet, CArgs>;
        static_assert(Checker::value,
                      "C function can not match expect abstract types");
    }
};

/**
 * Store type checked function ptr. The expected argument types
 * are specified by literal template arguments `Args`. Type check
 * errors would just raise a compile-time error.
 */
template <typename... Args>
struct TypeAnnotatedFuncPtrImpl<std::tuple<Args...>> {
    using GetTypeF =
        typename std::function<void(node::NodeManager*, node::TypeNode**)>;

    // TypeAnnotatedFuncPtr can only be bulit from non-void return type
    // Extra return type information should be provided for return-by-arg
    // function.
    template <typename CRet, typename... CArgs>
    TypeAnnotatedFuncPtrImpl(CRet (*fn)(CArgs...)) {  // NOLINT
        // Check signature, assume return type is same
        StaticFuncTypeCheck<typename CCallDataTypeTrait<CRet>::LiteralTag,
                            std::tuple<Args...>, CRet,
                            std::tuple<CArgs...>>::check();

        this->ptr = reinterpret_cast<void*>(fn);
        this->return_by_arg = false;
        this->return_nullable = false;
        this->get_ret_type_func = [](node::NodeManager* nm,
                                     node::TypeNode** ret) {
            *ret =
                DataTypeTrait<typename CCallDataTypeTrait<CRet>::LiteralTag>::
                    to_type_node(nm);
        };
    }

    // Create checked instance given abstract literal return type
    template <typename Ret, typename... CArgs>
    static auto RBA(void (*fn)(CArgs...)) {  // NOLINT
        // Check signature
        StaticFuncTypeCheck<Ret, std::tuple<Args...>, void,
                            std::tuple<CArgs...>>::check();

        TypeAnnotatedFuncPtrImpl<std::tuple<Args...>> res;
        res.ptr = reinterpret_cast<void*>(fn);
        res.return_by_arg = true;
        res.return_nullable = IsNullableTrait<Ret>::value;
        res.get_ret_type_func = [](node::NodeManager* nm,
                                   node::TypeNode** ret) {
            *ret = DataTypeTrait<Ret>::to_type_node(nm);
        };
        return res;
    }

    template <typename A1>
    TypeAnnotatedFuncPtrImpl(void (*fn)(A1*)) {  // NOLINT
        *this = RBA<typename CCallDataTypeTrait<A1*>::LiteralTag, A1*>(fn);
    }

    template <typename A1, typename A2>
    TypeAnnotatedFuncPtrImpl(void (*fn)(A1, A2*)) {  // NOLINT
        *this = RBA<typename CCallDataTypeTrait<A2*>::LiteralTag, A1, A2*>(fn);
    }

    template <typename A1, typename A2, typename A3>
    TypeAnnotatedFuncPtrImpl(void (*fn)(A1, A2, A3*)) {  // NOLINT
        *this =
            RBA<typename CCallDataTypeTrait<A3*>::LiteralTag, A1, A2, A3*>(fn);
    }

    template <typename A1, typename A2, typename A3, typename A4>
    TypeAnnotatedFuncPtrImpl(void (*fn)(A1, A2, A3, A4*)) {  // NOLINT
        *this =
            RBA<typename CCallDataTypeTrait<A4*>::LiteralTag, A1, A2, A3, A4*>(
                fn);
    }

    TypeAnnotatedFuncPtrImpl() {}

    void* ptr;
    bool return_by_arg;
    bool return_nullable;
    GetTypeF get_ret_type_func;
};

// used to instantiate tuple type from template param pack
template <typename... Args>
struct TypeAnnotatedFuncPtr {
    using type = TypeAnnotatedFuncPtrImpl<std::tuple<Args...>>;
};

class ExternalFuncRegistryHelper
    : public UDFRegistryHelper<ExternalFuncRegistry> {
 public:
    explicit ExternalFuncRegistryHelper(const std::string& basename,
                                        UDFLibrary* library)
        : UDFRegistryHelper<ExternalFuncRegistry>(basename, library) {}

    ~ExternalFuncRegistryHelper() {
        if (args_specified_) {
            finalize();
        }
    }

    template <typename Ret>
    ExternalFuncRegistryHelper& returns() {
        return_type_ = DataTypeTrait<Ret>::to_type_node(node_manager());
        return_nullable_ = IsNullableTrait<Ret>::value;
        return *this;
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& args(
        const std::string& name,
        const typename TypeAnnotatedFuncPtr<Args...>::type& fn_ptr) {
        args<Args...>(name, fn_ptr.ptr);
        update_return_info<Args...>(fn_ptr);
        return *this;
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& args(
        const typename TypeAnnotatedFuncPtr<Args...>::type& fn_ptr) {
        args<Args...>(fn_ptr.ptr);
        update_return_info<Args...>(fn_ptr);
        return *this;
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& args(const std::string& name, void* fn_ptr) {
        if (args_specified_) {
            finalize();
        }
        args_specified_ = true;
        fn_name_ = name;
        fn_ptr_ = fn_ptr;
        arg_types_ = {DataTypeTrait<Args>::to_type_node(node_manager())...};
        arg_nullable_ = {IsNullableTrait<Args>::value...};
        variadic_pos_ = -1;
        return *this;
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& args(void* fn_ptr) {
        std::string fn_name = name();
        for (auto param_name :
             {DataTypeTrait<Args>::to_type_node(node_manager())
                  ->GetName()...}) {
            fn_name.append(".").append(param_name);
        }
        return args<Args...>(fn_name, fn_ptr);
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& variadic_args(const std::string& name,
                                              void* fn_ptr) {
        if (args_specified_) {
            finalize();
        }
        fn_name_ = name;
        fn_ptr_ = fn_ptr;
        args_specified_ = true;
        arg_types_ = {DataTypeTrait<Args>::to_type_node(node_manager())...};
        arg_nullable_ = {IsNullableTrait<Args>::value...};
        variadic_pos_ = sizeof...(Args);
        return *this;
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& variadic_args(void* fn_ptr) {
        std::string fn_name = name();
        for (auto param_name :
             {DataTypeTrait<Args>::to_type_node(node_manager())
                  ->GetName()...}) {
            fn_name.append(".").append(param_name);
        }
        return variadic_args<Args...>(fn_name, fn_ptr);
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& variadic_args(
        const std::string& name,
        const typename TypeAnnotatedFuncPtr<Args...>::type& fn_ptr) {
        variadic_args<Args...>(name, fn_ptr.ptr);
        update_return_info(fn_ptr);
        return *this;
    }

    template <typename... Args>
    ExternalFuncRegistryHelper& variadic_args(
        const typename TypeAnnotatedFuncPtr<Args...>::type& fn_ptr) {
        variadic_args<Args...>(fn_ptr.ptr);
        update_return_info(fn_ptr);
        return *this;
    }

    ExternalFuncRegistryHelper& return_by_arg(bool flag) {
        return_by_arg_ = flag;
        return *this;
    }

    ExternalFuncRegistryHelper& doc(const std::string& str) {
        SetDoc(str);
        return *this;
    }

    node::ExternalFnDefNode* cur_def() const { return cur_def_; }

    void finalize() {
        if (return_type_ == nullptr) {
            LOG(WARNING) << "No return type specified for "
                         << " udf registry " << name();
            return;
        }
        auto def = node_manager()->MakeExternalFnDefNode(
            fn_name_, fn_ptr_, return_type_, return_nullable_, arg_types_,
            arg_nullable_, variadic_pos_, return_by_arg_);
        cur_def_ = def;

        auto registry = std::make_shared<ExternalFuncRegistry>(name(), def);
        library()->AddExternalSymbol(fn_name_, fn_ptr_);
        this->InsertRegistry(arg_types_, variadic_pos_ >= 0, registry);
        reset();
    }

 private:
    template <typename... Args>
    void update_return_info(
        const typename TypeAnnotatedFuncPtr<Args...>::type& fn_ptr) {
        node::TypeNode* dtype = nullptr;
        fn_ptr.get_ret_type_func(node_manager(), &dtype);
        if (dtype != nullptr) {
            return_type_ = dtype;
        }
        return_by_arg_ = fn_ptr.return_by_arg;
        return_nullable_ = fn_ptr.return_nullable;
    }

    void reset() {
        fn_name_ = "";
        fn_ptr_ = nullptr;
        args_specified_ = false;
        arg_types_.clear();
        arg_nullable_.clear();
        return_type_ = nullptr;
        return_nullable_ = false;
        variadic_pos_ = -1;
    }

    std::string fn_name_;
    void* fn_ptr_;
    bool args_specified_ = false;
    std::vector<const node::TypeNode*> arg_types_;
    std::vector<int> arg_nullable_;
    node::TypeNode* return_type_ = nullptr;
    bool return_nullable_ = false;
    int variadic_pos_ = -1;
    bool return_by_arg_ = false;

    node::ExternalFnDefNode* cur_def_ = nullptr;
};

template <template <typename> typename FTemplate>
class ExternalTemplateFuncRegistryHelper {
 public:
    ExternalTemplateFuncRegistryHelper(const std::string& name,
                                       UDFLibrary* library)
        : name_(name),
          library_(library),
          helper_(library_->RegisterExternal(name_)) {}

    template <typename... Args>
    ExternalTemplateFuncRegistryHelper& args_in() {
        cur_defs_ = {RegisterSingle<Args, typename FTemplate<Args>::Args>()(
            helper_, &FTemplate<Args>::operator())...};
        for (auto def : cur_defs_) {
            def->SetReturnByArg(return_by_arg_);
        }
        return *this;
    }

    ExternalTemplateFuncRegistryHelper& return_by_arg(bool flag) {
        return_by_arg_ = flag;
        for (auto def : cur_defs_) {
            def->SetReturnByArg(flag);
        }
        return *this;
    }

    auto& doc(const std::string& str) {
        helper_.doc(str);
        return *this;
    }

 private:
    template <typename T>
    using LiteralTag = typename CCallDataTypeTrait<T>::LiteralTag;

    template <typename T, typename... CArgs>
    struct FTemplateInst {
        static auto fcompute(CArgs... args) { return FTemplate<T>()(args...); }
    };

    template <typename T, typename>
    struct RegisterSingle;  // prmiary decl

    template <typename T, typename... Args>
    struct RegisterSingle<T, std::tuple<Args...>> {
        template <typename CRet, typename... CArgs>
        node::ExternalFnDefNode* operator()(
            ExternalFuncRegistryHelper& helper,    // NOLINT
            CRet (FTemplate<T>::*fn)(CArgs...)) {  // NOLINT
            helper.args<Args...>(FTemplateInst<T, CArgs...>::fcompute)
                .finalize();
            return helper.cur_def();
        }
    };

    std::string name_;
    UDFLibrary* library_;
    bool return_by_arg_ = false;
    std::vector<node::ExternalFnDefNode*> cur_defs_;
    ExternalFuncRegistryHelper helper_;
};

class SimpleUDFRegistry : public UDFRegistry {
 public:
    explicit SimpleUDFRegistry(const std::string& name,
                               node::UDFDefNode* fn_def)
        : UDFRegistry(name), fn_def_(fn_def) {}

    Status ResolveFunction(UDFResolveContext* ctx,
                           node::FnDefNode** result) override;

 private:
    node::UDFDefNode* fn_def_;
};

struct UDAFDefGen {
    std::shared_ptr<ExprUDFGenBase> init_gen = nullptr;
    std::shared_ptr<UDFRegistry> update_gen = nullptr;
    std::shared_ptr<UDFRegistry> merge_gen = nullptr;
    std::shared_ptr<UDFRegistry> output_gen = nullptr;
    node::TypeNode* state_type = nullptr;
    bool state_nullable = false;
};

class UDAFRegistry : public UDFRegistry {
 public:
    explicit UDAFRegistry(const std::string& name, const UDAFDefGen& udaf_gen)
        : UDFRegistry(name), udaf_gen_(udaf_gen) {}

    Status ResolveFunction(UDFResolveContext* ctx,
                           node::FnDefNode** result) override;

 private:
    UDAFDefGen udaf_gen_;
};

template <typename OUT, typename ST, typename... IN>
class UDAFRegistryHelperImpl;

class UDAFRegistryHelper : public UDFRegistryHelper<UDAFRegistry> {
 public:
    explicit UDAFRegistryHelper(const std::string& name, UDFLibrary* library)
        : UDFRegistryHelper<UDAFRegistry>(name, library) {}

    template <typename OUT, typename ST, typename... IN>
    UDAFRegistryHelperImpl<OUT, ST, IN...> templates();

    auto& doc(const std::string doc) {
        SetDoc(doc);
        return *this;
    }
};

template <typename OUT, typename ST, typename... IN>
class UDAFRegistryHelperImpl : UDFRegistryHelper<UDAFRegistry> {
 public:
    explicit UDAFRegistryHelperImpl(const std::string& name,
                                    UDFLibrary* library)
        : UDFRegistryHelper(name, library),
          elem_tys_(
              {DataTypeTrait<IN>::to_type_node(library->node_manager())...}),
          elem_nullable_({IsNullableTrait<IN>::value...}),
          state_ty_(DataTypeTrait<ST>::to_type_node(library->node_manager())),
          state_nullable_(IsNullableTrait<ST>::value),
          output_ty_(DataTypeTrait<OUT>::to_type_node(library->node_manager())),
          output_nullable_(IsNullableTrait<OUT>::value) {
        // specify update function argument types
        update_tys_.push_back(state_ty_);
        update_nullable_.push_back(state_nullable_);
        update_tags_.push_back(state_ty_->GetName());
        for (size_t i = 0; i < elem_tys_.size(); ++i) {
            update_tys_.push_back(elem_tys_[i]);
            update_nullable_.push_back(elem_nullable_[i]);
            update_tags_.push_back(elem_tys_[i]->GetName());
        }
    }

    ~UDAFRegistryHelperImpl() { finalize(); }

    // Start next registry types
    template <typename NewOUT, typename NewST, typename... NewIN>
    UDAFRegistryHelperImpl<NewOUT, NewST, NewIN...> templates() {
        finalize();
        auto helper_impl =
            UDAFRegistryHelperImpl<NewOUT, NewST, NewIN...>(name(), library());
        helper_impl.doc(this->GetDoc());
        return helper_impl;
    }

    UDAFRegistryHelperImpl& init(const std::string& fname) {
        udaf_gen_.init_gen =
            std::make_shared<ExprUDFGen>([fname](UDFResolveContext* ctx) {
                return ctx->node_manager()->MakeFuncNode(fname, {}, nullptr);
            });
        return *this;
    }

    UDAFRegistryHelperImpl& init(const std::string& fname, void* fn_ptr) {
        auto fn = dynamic_cast<node::ExternalFnDefNode*>(
            library()->node_manager()->MakeExternalFnDefNode(
                fname, fn_ptr, state_ty_, state_nullable_, {}, {}, -1, false));

        udaf_gen_.init_gen =
            std::make_shared<ExprUDFGen>([fn](UDFResolveContext* ctx) {
                return ctx->node_manager()->MakeFuncNode(fn, {}, nullptr);
            });
        library()->AddExternalSymbol(fname, fn_ptr);
        return *this;
    }

    UDAFRegistryHelperImpl& init(
        const std::string& fname,
        const typename TypeAnnotatedFuncPtr<>::type& fn_ptr) {
        node::TypeNode* ret_type = nullptr;
        fn_ptr.get_ret_type_func(library()->node_manager(), &ret_type);

        if (ret_type == nullptr) {
            LOG(WARNING) << "Fail to get return type of function ptr";
            return *this;
        } else if (!ret_type->Equals(state_ty_) ||
                   (fn_ptr.return_nullable && !state_nullable_)) {
            LOG(WARNING)
                << "Illegal input type of external init typed function '"
                << fname << "': expected "
                << (state_nullable_ ? "nullable " : "") << state_ty_->GetName()
                << " but get " << (fn_ptr.return_nullable ? "nullable " : "")
                << ret_type->GetName();
            return *this;
        }
        auto fn = library()->node_manager()->MakeExternalFnDefNode(
            fname, fn_ptr.ptr, state_ty_, state_nullable_, {}, {}, -1,
            fn_ptr.return_by_arg);
        udaf_gen_.init_gen =
            std::make_shared<ExprUDFGen<>>([fn](UDFResolveContext* ctx) {
                return ctx->node_manager()->MakeFuncNode(fn, {}, nullptr);
            });
        library()->AddExternalSymbol(fname, fn_ptr.ptr);
        return *this;
    }

    UDAFRegistryHelperImpl& const_init(const ST& value) {
        udaf_gen_.init_gen =
            std::make_shared<ExprUDFGen<>>([value](UDFResolveContext* ctx) {
                return DataTypeTrait<ST>::to_const(ctx->node_manager(), value);
            });
        return *this;
    }

    UDAFRegistryHelperImpl& const_init(const std::string& str) {
        udaf_gen_.init_gen =
            std::make_shared<ExprUDFGen<>>([str](UDFResolveContext* ctx) {
                return DataTypeTrait<ST>::to_const(ctx->node_manager(), str);
            });
        return *this;
    }

    UDAFRegistryHelperImpl& update(const std::string& fname) {
        auto registry = library()->Find(fname, update_tys_);
        if (registry != nullptr) {
            udaf_gen_.update_gen = registry;
        } else {
            std::stringstream ss;
            for (size_t i = 0; i < update_tys_.size(); ++i) {
                if (update_tys_[i] != nullptr) {
                    ss << update_tys_[i]->GetName();
                } else {
                    ss << "?";
                }
                if (i < update_tys_.size() - 1) {
                    ss << ", ";
                }
            }
            LOG(WARNING) << "Fail to find udaf registry " << fname << "<"
                         << ss.str() << ">";
        }
        return *this;
    }

    UDAFRegistryHelperImpl& update(
        const std::function<
            Status(UDFResolveContext* ctx, const ExprAttrNode*,
                   typename std::pair<IN, const ExprAttrNode*>::second_type...,
                   ExprAttrNode*)>& infer,
        const std::function<
            Status(codegen::CodeGenContext*, codegen::NativeValue,
                   typename std::pair<IN, codegen::NativeValue>::second_type...,
                   codegen::NativeValue*)>& gen) {
        auto llvm_gen = std::make_shared<LLVMUDFGen<ST, IN...>>(gen, infer);
        auto registry =
            std::make_shared<LLVMUDFRegistry>(name() + "@update", llvm_gen);
        udaf_gen_.update_gen = registry;
        return *this;
    }

    UDAFRegistryHelperImpl& update(
        const std::function<node::ExprNode*(
            UDFResolveContext*, node::ExprNode*,
            typename std::pair<IN, node::ExprNode*>::second_type...)>& gen) {
        auto expr_gen = std::make_shared<ExprUDFGen<ST, IN...>>(gen);
        auto registry =
            std::make_shared<ExprUDFRegistry>(name() + "@update", expr_gen);
        udaf_gen_.update_gen = registry;
        return *this;
    }

    UDAFRegistryHelperImpl& update(const std::string& fname, void* fn_ptr,
                                   bool return_by_arg = false) {
        auto fn = dynamic_cast<node::ExternalFnDefNode*>(
            library()->node_manager()->MakeExternalFnDefNode(
                fname, fn_ptr, state_ty_, state_nullable_, update_tys_,
                update_nullable_, -1, return_by_arg));
        auto registry = std::make_shared<ExternalFuncRegistry>(fname, fn);
        udaf_gen_.update_gen = registry;
        library()->AddExternalSymbol(fname, fn_ptr);
        return *this;
    }

    UDAFRegistryHelperImpl& update(
        const std::string& fname,
        const typename TypeAnnotatedFuncPtr<ST, IN...>::type& fn_ptr) {
        node::TypeNode* ret_type = nullptr;
        fn_ptr.get_ret_type_func(library()->node_manager(), &ret_type);
        if (ret_type == nullptr) {
            LOG(WARNING) << "Fail to get return type of function ptr";
            return *this;
        } else if (!ret_type->Equals(state_ty_) ||
                   (fn_ptr.return_nullable && !state_nullable_)) {
            LOG(WARNING)
                << "Illegal return type of external update typed function '"
                << fname << "': expected "
                << (state_nullable_ ? "nullable " : "") << state_ty_->GetName()
                << " but get " << (fn_ptr.return_nullable ? "nullable " : "")
                << ret_type->GetName();
            return *this;
        }
        return update(fname, fn_ptr.ptr, fn_ptr.return_by_arg);
    }

    UDAFRegistryHelperImpl& merge(const std::string& fname) {
        auto registry = library()->Find(fname, {state_ty_, state_ty_});
        if (registry != nullptr) {
            udaf_gen_.merge_gen = registry;
        } else {
            std::string state_ty_name =
                state_ty_ == nullptr ? "?" : state_ty_->GetName();
            LOG(WARNING) << "Fail to find udaf registry " << fname << "<"
                         << state_ty_name << ", " << state_ty_name << ">";
        }
        return *this;
    }

    UDAFRegistryHelperImpl& merge(const std::string& fname, void* fn_ptr) {
        auto fn = dynamic_cast<node::ExternalFnDefNode*>(
            library()->node_manager()->MakeExternalFnDefNode(
                fname, fn_ptr, state_ty_, state_nullable_,
                {state_ty_, state_ty_}, {state_nullable_, state_nullable_}, -1,
                false));
        auto registry = std::make_shared<ExternalFuncRegistry>(fname, fn);
        udaf_gen_.merge_gen = registry;
        library()->AddExternalSymbol(fname, fn_ptr);
        return *this;
    }

    UDAFRegistryHelperImpl& output(const std::string& fname) {
        auto registry = library()->Find(fname, {state_ty_});
        if (registry != nullptr) {
            udaf_gen_.output_gen = registry;
        } else {
            std::string state_ty_name =
                state_ty_ == nullptr ? "?" : state_ty_->GetName();
            LOG(WARNING) << "Fail to find udaf registry " << fname << "<"
                         << state_ty_name << ">";
        }
        return *this;
    }

    UDAFRegistryHelperImpl& output(const std::string& fname, void* fn_ptr,
                                   bool return_by_arg = false) {
        auto fn = dynamic_cast<node::ExternalFnDefNode*>(
            library()->node_manager()->MakeExternalFnDefNode(
                fname, fn_ptr, output_ty_, output_nullable_, {state_ty_},
                {state_nullable_}, -1, return_by_arg));
        auto registry = std::make_shared<ExternalFuncRegistry>(fname, fn);
        auto state_tag = state_ty_->GetName();
        udaf_gen_.output_gen = registry;
        library()->AddExternalSymbol(fname, fn_ptr);
        return *this;
    }

    UDAFRegistryHelperImpl& output(
        const std::string& fname,
        const typename TypeAnnotatedFuncPtr<ST>::type& fn_ptr) {
        node::TypeNode* ret_type = nullptr;
        fn_ptr.get_ret_type_func(library()->node_manager(), &ret_type);
        if (ret_type == nullptr) {
            LOG(WARNING) << "Fail to get return type of function ptr";
            return *this;
        } else if (!ret_type->Equals(output_ty_)) {
            LOG(WARNING)
                << "Illegal return type of external update typed function '"
                << fname << "': expected "
                << (state_nullable_ ? "nullable " : "") << state_ty_->GetName()
                << " but get " << (fn_ptr.return_nullable ? "nullable " : "")
                << ret_type->GetName();
            return *this;
        }
        return output(fname, fn_ptr.ptr, fn_ptr.return_by_arg);
    }

    UDAFRegistryHelperImpl& output(
        const std::function<node::ExprNode*(UDFResolveContext*,
                                            node::ExprNode*)>& gen) {
        auto expr_gen = std::make_shared<ExprUDFGen<ST>>(gen);
        auto registry =
            std::make_shared<ExprUDFRegistry>(name() + "@output", expr_gen);
        udaf_gen_.output_gen = registry;
        return *this;
    }

    void finalize() {
        if (elem_tys_.empty()) {
            LOG(WARNING) << "UDAF must take at least one input";
            return;
        }
        if (udaf_gen_.update_gen == nullptr) {
            LOG(WARNING) << "Update function not specified for " << name();
            return;
        }
        if (udaf_gen_.init_gen == nullptr) {
            if (!(elem_tys_.size() == 1 && elem_tys_[0]->Equals(state_ty_))) {
                LOG(WARNING) << "No init expr provided but input "
                             << "type does not equal to state type";
                return;
            }
        }
        udaf_gen_.state_type = state_ty_;
        udaf_gen_.state_nullable = state_nullable_;
        std::vector<const node::TypeNode*> input_list_types;
        for (auto elem_ty : elem_tys_) {
            input_list_types.push_back(
                library()->node_manager()->MakeTypeNode(node::kList, elem_ty));
        }
        auto registry = std::make_shared<UDAFRegistry>(name(), udaf_gen_);
        this->InsertRegistry(input_list_types, false, registry);
        library()->SetIsUDAF(name(), sizeof...(IN));
    }

    UDAFRegistryHelperImpl& doc(const std::string& doc) {
        SetDoc(doc);
        return *this;
    }

 private:
    std::vector<const node::TypeNode*> elem_tys_;
    std::vector<int> elem_nullable_;
    node::TypeNode* state_ty_;
    bool state_nullable_;
    node::TypeNode* output_ty_;
    bool output_nullable_;

    UDAFDefGen udaf_gen_;
    std::vector<const node::TypeNode*> update_tys_;
    std::vector<int> update_nullable_;
    std::vector<std::string> update_tags_;
};

template <typename OUT, typename ST, typename... IN>
UDAFRegistryHelperImpl<OUT, ST, IN...> UDAFRegistryHelper::templates() {
    auto helper_impl =
        UDAFRegistryHelperImpl<OUT, ST, IN...>(name(), library());
    helper_impl.doc(this->GetDoc());
    return helper_impl;
}

template <template <typename> typename FTemplate>
class UDAFTemplateRegistryHelper : public UDFRegistryHelper<UDAFRegistry> {
 public:
    UDAFTemplateRegistryHelper(const std::string& name, UDFLibrary* library)
        : UDFRegistryHelper<UDAFRegistry>(name, library),
          helper_(name, library) {}

    template <typename... Args>
    UDAFTemplateRegistryHelper& args_in() {
        results_ = {RegisterSingle<Args>(helper_)...};
        return *this;
    }

    auto& doc(const std::string& str) {
        helper_.doc(str);
        return *this;
    }

 private:
    template <typename T>
    int RegisterSingle(UDAFRegistryHelper& helper) {  // NOLINT
        FTemplate<T> inst;
        inst(helper);
        return 0;
    }

    UDAFRegistryHelper helper_;
    std::vector<int> results_;
};

}  // namespace udf
}  // namespace fesql

#endif  // SRC_UDF_UDF_REGISTRY_H_
