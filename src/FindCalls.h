#ifndef FIND_CALLS_H
#define FIND_CALLS_H

/** \file
 *
 * Defines analyses to extract the functions called a function.
 */

#include <map>
#include <string>

#include "Expr.h"

namespace Halide {
namespace Internal {

class Function;
class Parameter;

/** Construct a map from name to Function definition object for all Halide
 *  functions called directly in the definition of the Function f, including
 *  in update definitions, update index expressions, and RDom extents. This map
 *  _does not_ include the Function f, unless it is called recursively by
 *  itself.
 */
std::map<std::string, Function> find_direct_calls(Function f);

/** Construct a map from name to Function definition object for all Halide
 *  functions called directly in the definition of the Function f, or
 *  indirectly in those functions' definitions, recursively. This map always
 *  _includes_ the Function f.
 */
std::map<std::string, Function> find_transitive_calls(Function f);

/** Find all Functions transitively referenced by f in any way and add
 * them to the given map. */
void populate_environment(Function f, std::map<std::string, Function> &env);

/** Find all Functions and parameters transitively referenced by f in any way and add
 * them to the given maps. */
void find_parameter_and_function_calls(Function f, std::map<std::string, Function> &env, std::map<std::string, Parameter> &par_res);

}  // namespace Internal
}  // namespace Halide

#endif
