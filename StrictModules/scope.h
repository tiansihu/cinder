// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#ifndef STRICTM_SCOPE_H
#define STRICTM_SCOPE_H
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "StrictModules/symbol_table.h"
namespace strictmod {

std::string mangle(const std::string& className, const std::string& name);

template <typename TVar, typename TScopeData>
class Scope {
 public:
  Scope(
      SymtableEntry scope,
      TScopeData data,
      bool invisible = false,
      bool hasAlternativeDict = false)
      : scope_(scope),
        vars_(std::make_shared<std::unordered_map<std::string, TVar>>()),
        data_(data),
        invisible_(invisible),
        hasAlternativeDict_(hasAlternativeDict) {}

  Scope(
      SymtableEntry scope,
      std::shared_ptr<std::unordered_map<std::string, TVar>> vars,
      TScopeData data,
      bool invisible = false,
      bool hasAlternativeDict = false)
      : scope_(scope),
        vars_(vars),
        data_(data),
        invisible_(invisible),
        hasAlternativeDict_(hasAlternativeDict) {}

  /* Interface for using scope as namespace
   *  set(key, value): set key to value, add new member if key is absent
   *  at: return by value, raise exception if key is absent
   *  erase: remove key, return true if anything is removed
   *  contains: returns whether key exists
   *
   *  If hasAlternativeDict_ is true, `TScopeData data` should
   *  implement the same interface which will be used instead
   */
  void set(const std::string& key, TVar value);
  TVar at(const std::string& key);
  bool erase(const std::string& key);
  bool contains(const std::string& key) const;

  const SymtableEntry& getSTEntry() const {
    return scope_;
  }

  bool isClassScope() const {
    return scope_.isClassScope();
  }

  bool isFunctionScope() const {
    return scope_.isFunctionScope();
  }

  bool isInvisible() const {
    return invisible_;
  }

  std::string getScopeName() const {
    return scope_.getTableName();
  }

  const TScopeData& getScopeData() const {
    return data_;
  }

  void setScopeData(TScopeData data) {
    data_ = std::move(data);
  }

 private:
  SymtableEntry scope_;
  std::shared_ptr<std::unordered_map<std::string, TVar>> vars_;
  TScopeData data_;
  bool invisible_;
  bool hasAlternativeDict_; // look into ScopeData instead of vars_ for dict
};

template <typename TVar, typename TScopeData>
class ScopeManager;

template <typename TVar, typename TScopeData>
class ScopeStack {
  typedef std::shared_ptr<std::unordered_map<std::string, TVar>> TMapPtr;
  typedef std::function<
      std::unique_ptr<Scope<TVar, TScopeData>>(SymtableEntry, TMapPtr)>
      ScopeFactory;

  typedef std::vector<std::shared_ptr<Scope<TVar, TScopeData>>> ScopeVector;

 public:
  ScopeStack(ScopeVector scopes, Symtable symbols, ScopeFactory factory)
      : scopes_(std::move(scopes)),
        symbols_(std::move(symbols)),
        scopeFactory_(factory),
        currentClass_() {}

  ScopeStack(
      Symtable symbols,
      ScopeFactory factory,
      std::shared_ptr<Scope<TVar, TScopeData>> topScope)
      : scopes_(),
        symbols_(std::move(symbols)),
        scopeFactory_(factory),
        currentClass_() {
    scopes_.push_back(topScope);
  }

  ScopeStack(
      Symtable symbols,
      ScopeFactory factory,
      std::unique_ptr<Scope<TVar, TScopeData>> topScope)
      : ScopeStack(
            std::move(symbols),
            factory,
            std::shared_ptr(std::move(topScope))) {}

  explicit ScopeStack(const ScopeStack<TVar, TScopeData>& rhs)
      : scopes_(rhs.scopes_),
        symbols_(rhs.symbols_),
        scopeFactory_(rhs.scopeFactory_),
        currentClass_(rhs.currentClass_) {}

  /** From this, get scope stack for function defined in the
   * current scope
   */
  ScopeStack getFunctionScope();

  /* use this for setting */
  void set(const std::string& key, TVar value);
  /* use this for reading, return nullopt if key doesn't exist */
  std::optional<TVar> at(const std::string& key) const;
  bool erase(const std::string& key);
  void clear();

  bool isGlobal(const std::string& key) const;
  bool isNonLocal(const std::string& key) const;

  void push(std::shared_ptr<Scope<TVar, TScopeData>> scope);
  void pop();

  ScopeManager<TVar, TScopeData> enterScopeByAst(
      stmt_ty key,
      TMapPtr vars = nullptr);
  ScopeManager<TVar, TScopeData> enterScopeByAst(
      mod_ty key,
      TMapPtr vars = nullptr);
  ScopeManager<TVar, TScopeData> enterScopeByAst(
      expr_ty key,
      TMapPtr vars = nullptr);

  ScopeManager<TVar, TScopeData> enterScope(
      std::unique_ptr<Scope<TVar, TScopeData>> scope,
      std::optional<std::string> currentClass = std::nullopt);

  std::optional<std::string> getCurrentClass(void) {
    return currentClass_;
  }
  void setCurrentClass(std::optional<std::string> className) {
    currentClass_ = std::move(className);
  }
  std::string mangleName(std::string name) const {
    if (!currentClass_) {
      return name;
    }
    return mangle(currentClass_.value(), std::move(name));
  }

  /*
   * Get the qualified name of the current scope, excluding
   * the first scope which is always 'top'
   */
  std::string getQualifiedScopeName() const {
    std::ostringstream ss;
    for (auto it = std::next(scopes_.begin()); it != scopes_.end(); ++it) {
      ss << (*it)->getScopeName();
      if (std::next(it) != scopes_.end()) {
        ss << ".";
      }
    }
    return ss.str();
  }

  const Symtable& getSymtable() const {
    return symbols_;
  }

  const Scope<TVar, TScopeData>& getCurrentScope() const {
    return *scopes_.back();
  }

  bool isClassScope() const;
  bool isGlobalScope() const;
  bool localContains(const std::string& key) const;
  void localSet(std::string key, TVar value);

 private:
  ScopeVector scopes_;
  Symtable symbols_;
  ScopeFactory scopeFactory_;
  std::optional<std::string> currentClass_;

  ScopeManager<TVar, TScopeData> enterScopeByAstBody(
      void* key,
      TMapPtr vars,
      std::optional<std::string> className = std::nullopt);
};

template <typename TVar, typename TScopeData>
class ScopeManager {
 public:
  ScopeManager(
      ScopeStack<TVar, TScopeData>& parent,
      std::shared_ptr<Scope<TVar, TScopeData>> scope,
      std::optional<std::string> currentClass)
      : parent_(parent), scope_(scope), oldClass_(parent_.getCurrentClass()) {
    parent_.push(scope);
    if (currentClass) {
      parent_.setCurrentClass(currentClass);
    }
  }

  ~ScopeManager() {
    parent_.pop();
    parent_.setCurrentClass(oldClass_);
  }

  std::shared_ptr<Scope<TVar, TScopeData>> getScope() {
    return scope_;
  }

 private:
  ScopeStack<TVar, TScopeData>& parent_;
  std::shared_ptr<Scope<TVar, TScopeData>> scope_;
  std::optional<std::string> oldClass_;
};

} // namespace strictmod

#include "StrictModules/scope_impl.h"

#define STRICTM_SCOPE_H
#endif // STRICTM_SCOPE_H
