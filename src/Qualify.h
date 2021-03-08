#ifndef HALIDE_QUALIFY_H
#define HALIDE_QUALIFY_H

/** \file
 *
 * Defines methods for prefixing names in an expression with a prefix string.
 */
#include <string>

#include "Expr.h"

namespace Halide {
namespace Internal {

/** Prefix all variable names in the given expression with the prefix string. */
Expr qualify(const std::string &prefix, const Expr &value);
std::vector<Annotation> qualify(const std::string &prefix, const std::vector<Annotation> &anns);

}  // namespace Internal
}  // namespace Halide

#endif
