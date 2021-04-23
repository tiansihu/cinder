#ifndef __STRICTM_SYMBOL_TABLE_H__
#define __STRICTM_SYMBOL_TABLE_H__

#include <memory>
#include <unordered_map>

#include "StrictModules/py_headers.h"
namespace strictmod {
using PySymtable = struct symtable;

std::string mangle(const std::string& className, const std::string& name);

struct PySymtableDeleter {
  /**
  Symtables are created using Python's symtable_new() method
  and must be deleted using PySymtable_Free. This is a custom
  deleter that does that and can be used in smart pointers
  */
  void operator()(PySymtable* p) {
    PySymtable_Free(p);
  }
};

class SymtableEntry;

/** Wrapper around the CPython symtable struct
 * to make accessing various functionalities easier
 */
class Symtable {
 public:
  Symtable(std::shared_ptr<PySymtable> symtable) : symtable_(symtable){};
  Symtable(std::unique_ptr<PySymtable, PySymtableDeleter> symtable)
      : symtable_(std::move(symtable)) {}

  SymtableEntry entryFromAst(void* key) const;

 private:
  std::shared_ptr<PySymtable> symtable_;
};

/** Properties of one symbol in the symbol table
 * Organized similarly to symtable.py
 */
class Symbol {
 public:
  Symbol(long flags) : flags_(flags) {
    scopeFlag_ = (flags_ >> SCOPE_OFFSET) & SCOPE_MASK;
  }

  bool is_global() const;
  bool is_nonlocal() const;
  bool is_local(void) const;

 private:
  long flags_;
  int scopeFlag_;
};

/** Wrapper around CPython PySTEntryObject
 * to make accessing various functionalities easier
 */
class SymtableEntry {
 public:
  SymtableEntry(PySTEntryObject* entry) : entry_(entry), symbolCache_() {}

  /* expects mangled name */
  const Symbol& getSymbol(const std::string& name) const;
  bool isClassScope(void) const;

 private:
  PySTEntryObject* entry_;
  mutable std::unordered_map<std::string, Symbol> symbolCache_;
};
} // namespace strictmod

#endif // __STRICTM_SYMBOL_TABLE_H__