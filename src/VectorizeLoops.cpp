#include <algorithm>
#include <utility>

#include "CSE.h"
#include "CodeGen_GPU_Dev.h"
#include "Deinterleave.h"
#include "ExprUsesVar.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Scope.h"
#include "Simplify.h"
#include "Solve.h"
#include "Substitute.h"
#include "VectorizeLoops.h"

namespace Halide {
namespace Internal {

using std::pair;
using std::string;
using std::vector;

namespace {

// For a given var, replace expressions like shuffle_vector(var, 4)
// with var.lane.4
class ReplaceShuffleVectors : public IRMutator {
    string var;

    using IRMutator::visit;

    Expr visit(const Shuffle *op) override {
        const Variable *v;
        if (op->indices.size() == 1 &&
            (v = op->vectors[0].as<Variable>()) &&
            v->name == var) {
            return Variable::make(op->type, var + ".lane." + std::to_string(op->indices[0]));
        } else {
            return IRMutator::visit(op);
        }
    }

public:
    ReplaceShuffleVectors(const string &v)
        : var(v) {
    }
};

/** Find the exact max and min lanes of a vector expression. Not
 * conservative like bounds_of_expr, but uses similar rules for some
 * common node types where it can be exact. Assumes any vector
 * variables defined externally also have .min_lane and .max_lane
 * versions in scope. */
Interval bounds_of_lanes(const Expr &e) {
    if (const Add *add = e.as<Add>()) {
        if (const Broadcast *b = add->b.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(add->a);
            return {ia.min + b->value, ia.max + b->value};
        } else if (const Broadcast *b = add->a.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(add->b);
            return {b->value + ia.min, b->value + ia.max};
        }
    } else if (const Sub *sub = e.as<Sub>()) {
        if (const Broadcast *b = sub->b.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(sub->a);
            return {ia.min - b->value, ia.max - b->value};
        } else if (const Broadcast *b = sub->a.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(sub->b);
            return {b->value - ia.max, b->value - ia.max};
        }
    } else if (const Mul *mul = e.as<Mul>()) {
        if (const Broadcast *b = mul->b.as<Broadcast>()) {
            if (is_positive_const(b->value)) {
                Interval ia = bounds_of_lanes(mul->a);
                return {ia.min * b->value, ia.max * b->value};
            } else if (is_negative_const(b->value)) {
                Interval ia = bounds_of_lanes(mul->a);
                return {ia.max * b->value, ia.min * b->value};
            }
        } else if (const Broadcast *b = mul->a.as<Broadcast>()) {
            if (is_positive_const(b->value)) {
                Interval ia = bounds_of_lanes(mul->b);
                return {b->value * ia.min, b->value * ia.max};
            } else if (is_negative_const(b->value)) {
                Interval ia = bounds_of_lanes(mul->b);
                return {b->value * ia.max, b->value * ia.min};
            }
        }
    } else if (const Div *div = e.as<Div>()) {
        if (const Broadcast *b = div->b.as<Broadcast>()) {
            if (is_positive_const(b->value)) {
                Interval ia = bounds_of_lanes(div->a);
                return {ia.min / b->value, ia.max / b->value};
            } else if (is_negative_const(b->value)) {
                Interval ia = bounds_of_lanes(div->a);
                return {ia.max / b->value, ia.min / b->value};
            }
        }
    } else if (const And *and_ = e.as<And>()) {
        if (const Broadcast *b = and_->b.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(and_->a);
            return {ia.min && b->value, ia.max && b->value};
        } else if (const Broadcast *b = and_->a.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(and_->b);
            return {ia.min && b->value, ia.max && b->value};
        }
    } else if (const Or *or_ = e.as<Or>()) {
        if (const Broadcast *b = or_->b.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(or_->a);
            return {ia.min && b->value, ia.max && b->value};
        } else if (const Broadcast *b = or_->a.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(or_->b);
            return {ia.min && b->value, ia.max && b->value};
        }
    } else if (const Min *min = e.as<Min>()) {
        if (const Broadcast *b = min->b.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(min->a);
            return {Min::make(ia.min, b->value), Min::make(ia.max, b->value)};
        } else if (const Broadcast *b = min->a.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(min->b);
            return {Min::make(ia.min, b->value), Min::make(ia.max, b->value)};
        }
    } else if (const Max *max = e.as<Max>()) {
        if (const Broadcast *b = max->b.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(max->a);
            return {Max::make(ia.min, b->value), Max::make(ia.max, b->value)};
        } else if (const Broadcast *b = max->a.as<Broadcast>()) {
            Interval ia = bounds_of_lanes(max->b);
            return {Max::make(ia.min, b->value), Max::make(ia.max, b->value)};
        }
    } else if (const Not *not_ = e.as<Not>()) {
        Interval ia = bounds_of_lanes(not_->a);
        return {!ia.max, !ia.min};
    } else if (const Ramp *r = e.as<Ramp>()) {
        Expr last_lane_idx = make_const(r->base.type(), r->lanes - 1);
        if (is_positive_const(r->stride)) {
            return {r->base, r->base + last_lane_idx * r->stride};
        } else if (is_negative_const(r->stride)) {
            return {r->base + last_lane_idx * r->stride, r->base};
        }
    } else if (const Broadcast *b = e.as<Broadcast>()) {
        return {b->value, b->value};
    } else if (const Variable *var = e.as<Variable>()) {
        return {Variable::make(var->type.element_of(), var->name + ".min_lane"),
                Variable::make(var->type.element_of(), var->name + ".max_lane")};
    } else if (const Let *let = e.as<Let>()) {
        Interval ia = bounds_of_lanes(let->value);
        Interval ib = bounds_of_lanes(let->body);
        if (expr_uses_var(ib.min, let->name + ".min_lane")) {
            ib.min = Let::make(let->name + ".min_lane", ia.min, ib.min);
        }
        if (expr_uses_var(ib.max, let->name + ".min_lane")) {
            ib.max = Let::make(let->name + ".min_lane", ia.min, ib.max);
        }
        if (expr_uses_var(ib.min, let->name + ".max_lane")) {
            ib.min = Let::make(let->name + ".max_lane", ia.max, ib.min);
        }
        if (expr_uses_var(ib.max, let->name + ".max_lane")) {
            ib.max = Let::make(let->name + ".max_lane", ia.max, ib.max);
        }
        if (expr_uses_var(ib.min, let->name)) {
            ib.min = Let::make(let->name, let->value, ib.min);
        }
        if (expr_uses_var(ib.max, let->name)) {
            ib.max = Let::make(let->name, let->value, ib.max);
        }
        return ib;
    }

    // Take the explicit min and max over the lanes
    Expr min_lane = extract_lane(e, 0);
    Expr max_lane = min_lane;
    for (int i = 1; i < e.type().lanes(); i++) {
        Expr next_lane = extract_lane(e, i);
        if (e.type().is_bool()) {
            min_lane = And::make(min_lane, next_lane);
            max_lane = Or::make(max_lane, next_lane);
        } else {
            min_lane = Min::make(min_lane, next_lane);
            max_lane = Max::make(max_lane, next_lane);
        }
    }
    return {min_lane, max_lane};
};

// Allocations inside vectorized loops grow an additional inner
// dimension to represent the separate copy of the allocation per
// vector lane. This means loads and stores to them need to be
// rewritten slightly.
class RewriteAccessToVectorAlloc : public IRMutator {
    Expr var;
    string alloc;
    int lanes;

    using IRMutator::visit;

    Expr mutate_index(const string &a, Expr index) {
        index = mutate(index);
        if (a == alloc) {
            return index * lanes + var;
        } else {
            return index;
        }
    }

    ModulusRemainder mutate_alignment(const string &a, const ModulusRemainder &align) {
        if (a == alloc) {
            return align * lanes;
        } else {
            return align;
        }
    }

    Expr visit(const Load *op) override {
        return Load::make(op->type, op->name, mutate_index(op->name, op->index),
                          op->image, op->param, mutate(op->predicate), mutate_alignment(op->name, op->alignment));
    }

    Stmt visit(const Store *op) override {
        return Store::make(op->name, mutate(op->value), mutate_index(op->name, op->index),
                           op->param, mutate(op->predicate), mutate_alignment(op->name, op->alignment));
    }

public:
    RewriteAccessToVectorAlloc(const string &v, string a, int l)
        : var(Variable::make(Int(32), v)), alloc(std::move(a)), lanes(l) {
    }
};

class UsesGPUVars : public IRVisitor {
private:
    using IRVisitor::visit;
    void visit(const Variable *op) override {
        if (CodeGen_GPU_Dev::is_gpu_var(op->name)) {
            debug(3) << "Found gpu loop var: " << op->name << "\n";
            uses_gpu = true;
        }
    }

public:
    bool uses_gpu = false;
};

bool uses_gpu_vars(const Expr &s) {
    UsesGPUVars uses;
    s.accept(&uses);
    return uses.uses_gpu;
}

class SerializeLoops : public IRMutator {
    Stmt visit(const For *op) override {
        if (op->for_type == ForType::Vectorized) {
            return For::make(op->name, mutate(op->min), mutate(op->extent),
                             ForType::Serial, op->device_api, mutate(op->body));
        }

        return IRMutator::visit(op);
    }
};

// Wrap a vectorized predicate around a Load/Store node.
class PredicateLoadStore : public IRMutator {
    string var;
    Expr vector_predicate;
    bool in_hexagon;
    const Target &target;
    int lanes;
    bool valid;
    bool vectorized;

    using IRMutator::visit;

    bool should_predicate_store_load(int bit_size) {
        if (in_hexagon) {
            internal_assert(target.features_any_of({Target::HVX_64, Target::HVX_128}))
                << "We are inside a hexagon loop, but the target doesn't have hexagon's features\n";
            return true;
        } else if (target.arch == Target::X86) {
            // Should only attempt to predicate store/load if the lane size is
            // no less than 4
            // TODO: disabling for now due to trunk LLVM breakage.
            // See: https://github.com/halide/Halide/issues/3534
            // return (bit_size == 32) && (lanes >= 4);
            return false;
        }
        // For other architecture, do not predicate vector load/store
        return false;
    }

    Expr merge_predicate(Expr pred, const Expr &new_pred) {
        if (pred.type().lanes() == new_pred.type().lanes()) {
            Expr res = simplify(pred && new_pred);
            return res;
        }
        valid = false;
        return pred;
    }

    Expr visit(const Load *op) override {
        valid = valid && should_predicate_store_load(op->type.bits());
        if (!valid) {
            return op;
        }

        Expr predicate, index;
        if (!op->index.type().is_scalar()) {
            internal_assert(op->predicate.type().lanes() == lanes);
            internal_assert(op->index.type().lanes() == lanes);

            predicate = mutate(op->predicate);
            index = mutate(op->index);
        } else if (expr_uses_var(op->index, var)) {
            predicate = mutate(Broadcast::make(op->predicate, lanes));
            index = mutate(Broadcast::make(op->index, lanes));
        } else {
            return IRMutator::visit(op);
        }

        predicate = merge_predicate(predicate, vector_predicate);
        if (!valid) {
            return op;
        }
        vectorized = true;
        return Load::make(op->type, op->name, index, op->image, op->param, predicate, op->alignment);
    }

    Stmt visit(const Store *op) override {
        valid = valid && should_predicate_store_load(op->value.type().bits());
        if (!valid) {
            return op;
        }

        Expr predicate, value, index;
        if (!op->index.type().is_scalar()) {
            internal_assert(op->predicate.type().lanes() == lanes);
            internal_assert(op->index.type().lanes() == lanes);
            internal_assert(op->value.type().lanes() == lanes);

            predicate = mutate(op->predicate);
            value = mutate(op->value);
            index = mutate(op->index);
        } else if (expr_uses_var(op->index, var)) {
            predicate = mutate(Broadcast::make(op->predicate, lanes));
            value = mutate(Broadcast::make(op->value, lanes));
            index = mutate(Broadcast::make(op->index, lanes));
        } else {
            return IRMutator::visit(op);
        }

        predicate = merge_predicate(predicate, vector_predicate);
        if (!valid) {
            return op;
        }
        vectorized = true;
        return Store::make(op->name, value, index, op->param, predicate, op->alignment);
    }

    Expr visit(const Call *op) override {
        // We should not vectorize calls with side-effects
        valid = valid && op->is_pure();
        return IRMutator::visit(op);
    }

public:
    PredicateLoadStore(string v, const Expr &vpred, bool in_hexagon, const Target &t)
        : var(std::move(v)), vector_predicate(vpred), in_hexagon(in_hexagon), target(t),
          lanes(vpred.type().lanes()), valid(true), vectorized(false) {
        internal_assert(lanes > 1);
    }

    bool is_vectorized() const {
        return valid && vectorized;
    }
};

struct VectorizedVar {
    string name;
    Expr min;
    int lanes;
};

// Substitutes a vector for a scalar var in a Stmt. Used on the
// body of every vectorized loop.
class VectorSubs : public IRMutator {
    std::vector<VectorizedVar> vectorized_vars;

    // What we're replacing it with. Usually a ramp.
    std::map<string, Expr> replacements;
    std::map<string, Expr> replacements_from_zero;

    std::map<string, std::map<string, Expr>> widened_vars;

    const Target &target;

    bool in_hexagon;  // Are we inside the hexagon loop?

    // A scope containing lets and letstmts whose values became
    // vectors.
    Scope<Expr> scope;

    // A stack of all containing lets. We need to reinject the scalar
    // version of them if we scalarize inner code.
    vector<pair<string, Expr>> containing_lets;

    // Widen an expression to the given number of lanes.
    Expr widen(Expr e, int lanes) {
        if (e.type().lanes() == lanes) {
            return e;
        } else if (lanes % e.type().lanes() == 0) {
            return Broadcast::make(e, lanes / e.type().lanes());
        } else {
            internal_error << "Mismatched vector lanes in VectorSubs " << e.type().lanes()
                           << " " << lanes << "\n";
        }
        return Expr();
    }

    using IRMutator::visit;

    Expr visit(const Cast *op) override {
        Expr value = mutate(op->value);
        if (value.same_as(op->value)) {
            return op;
        } else {
            Type t = op->type.with_lanes(value.type().lanes());
            return Cast::make(t, value);
        }
    }

    Expr visit(const Variable *op) override {
        if (replacements.count(op->name) > 0) {
            return replacements[op->name];
        } else if (scope.contains(op->name)) {
            string widened_name = op->name + ".widened." + vectorized_vars.back().name;
            Expr mutated;
            // Depending on the current loop level, we may need to
            // widen variable differently.
            if (widened_vars[op->name].count(widened_name) > 0) {
                mutated = widened_vars[op->name][widened_name];
            } else {
                mutated = mutate(scope.get(op->name));
                widened_vars[op->name][widened_name] = mutated;
            }
            return Variable::make(mutated.type(), widened_name);
        } else {
            return op;
        }
    }

    template<typename T>
    Expr mutate_binary_operator(const T *op) {
        Expr a = mutate(op->a), b = mutate(op->b);
        if (a.same_as(op->a) && b.same_as(op->b)) {
            return op;
        } else {
            int w = std::max(a.type().lanes(), b.type().lanes());
            return T::make(widen(a, w), widen(b, w));
        }
    }

    Expr visit(const Add *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Sub *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Mul *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Div *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Mod *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Min *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Max *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const EQ *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const NE *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const LT *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const LE *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const GT *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const GE *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const And *op) override {
        return mutate_binary_operator(op);
    }
    Expr visit(const Or *op) override {
        return mutate_binary_operator(op);
    }

    Expr visit(const Ramp *op) override {
        Expr base = mutate(op->base);
        Expr stride = mutate(op->stride);
        return Ramp::make(base, stride, op->lanes);
    }

    Expr visit(const Select *op) override {
        Expr condition = mutate(op->condition);
        Expr true_value = mutate(op->true_value);
        Expr false_value = mutate(op->false_value);
        if (condition.same_as(op->condition) &&
            true_value.same_as(op->true_value) &&
            false_value.same_as(op->false_value)) {
            return op;
        } else {
            int lanes = std::max(true_value.type().lanes(), false_value.type().lanes());
            lanes = std::max(lanes, condition.type().lanes());
            // Widen the true and false values, but we don't have to widen the condition
            true_value = widen(true_value, lanes);
            false_value = widen(false_value, lanes);
            return Select::make(condition, true_value, false_value);
        }
    }

    Expr visit(const Load *op) override {
        Expr predicate = mutate(op->predicate);
        Expr index = mutate(op->index);

        if (predicate.same_as(op->predicate) && index.same_as(op->index)) {
            return op;
        } else {
            int w = index.type().lanes();
            predicate = widen(predicate, w);
            return Load::make(op->type.with_lanes(w), op->name, index, op->image,
                              op->param, predicate, op->alignment);
        }
    }

    Expr visit(const Call *op) override {
        // Widen the call by changing the lanes of all of its
        // arguments and its return type
        vector<Expr> new_args(op->args.size());
        bool changed = false;

        // Mutate the args
        int max_lanes = 0;
        for (size_t i = 0; i < op->args.size(); i++) {
            Expr old_arg = op->args[i];
            Expr new_arg = mutate(old_arg);
            if (!new_arg.same_as(old_arg)) changed = true;
            new_args[i] = new_arg;
            max_lanes = std::max(new_arg.type().lanes(), max_lanes);
        }

        if (!changed) {
            return op;
        } else if (op->name == Call::trace) {
            const int64_t *event = as_const_int(op->args[6]);
            internal_assert(event != nullptr);
            if (*event == halide_trace_begin_realization || *event == halide_trace_end_realization) {
                // Call::trace vectorizes uniquely for begin/end realization, because the coordinates
                // for these are actually min/extent pairs; we need to maintain the proper dimensionality
                // count and instead aggregate the widened values into a single pair.
                for (size_t i = 1; i <= 2; i++) {
                    const Call *call = new_args[i].as<Call>();
                    internal_assert(call && call->is_intrinsic(Call::make_struct));
                    if (i == 1) {
                        // values should always be empty for these events
                        internal_assert(call->args.empty());
                        continue;
                    }
                    vector<Expr> call_args(call->args.size());
                    for (size_t j = 0; j < call_args.size(); j += 2) {
                        Expr min_v = widen(call->args[j], max_lanes);
                        Expr extent_v = widen(call->args[j + 1], max_lanes);
                        Expr min_scalar = extract_lane(min_v, 0);
                        Expr max_scalar = min_scalar + extract_lane(extent_v, 0);
                        for (int k = 1; k < max_lanes; ++k) {
                            Expr min_k = extract_lane(min_v, k);
                            Expr extent_k = extract_lane(extent_v, k);
                            min_scalar = min(min_scalar, min_k);
                            max_scalar = max(max_scalar, min_k + extent_k);
                        }
                        call_args[j] = min_scalar;
                        call_args[j + 1] = max_scalar - min_scalar;
                    }
                    new_args[i] = Call::make(call->type.element_of(), Call::make_struct, call_args, Call::Intrinsic);
                }
            } else {
                // Call::trace vectorizes uniquely, because we want a
                // single trace call for the entire vector, instead of
                // scalarizing the call and tracing each element.
                for (size_t i = 1; i <= 2; i++) {
                    // Each struct should be a struct-of-vectors, not a
                    // vector of distinct structs.
                    const Call *call = new_args[i].as<Call>();
                    internal_assert(call && call->is_intrinsic(Call::make_struct));
                    // Widen the call args to have the same lanes as the max lanes found
                    vector<Expr> call_args(call->args.size());
                    for (size_t j = 0; j < call_args.size(); j++) {
                        call_args[j] = widen(call->args[j], max_lanes);
                    }
                    new_args[i] = Call::make(call->type.element_of(), Call::make_struct,
                                             call_args, Call::Intrinsic);
                }
                // One of the arguments to the trace helper
                // records the number of vector lanes in the type being
                // stored.
                new_args[5] = max_lanes;
                // One of the arguments to the trace helper
                // records the number entries in the coordinates (which we just widened)
                if (max_lanes > 1) {
                    new_args[9] = new_args[9] * max_lanes;
                }
            }
            return Call::make(op->type, Call::trace, new_args, op->call_type);
        } else {
            // Widen the args to have the same lanes as the max lanes found
            for (size_t i = 0; i < new_args.size(); i++) {
                new_args[i] = widen(new_args[i], max_lanes);
            }
            return Call::make(op->type.with_lanes(max_lanes), op->name, new_args,
                              op->call_type, op->func, op->value_index, op->image, op->param);
        }
    }

    Expr visit(const Let *op) override {
        // Vectorize the let value and check to see if it was vectorized by
        // this mutator. The type of the expression might already be vector
        // width.
        Expr new_value = mutate(op->value);
        bool was_vectorized = (!op->value.type().is_vector() &&
                               new_value.type().is_vector());

        // If the value was vectorized by this mutator, add a new name to
        // the scope for the vectorized value expression.
        if (was_vectorized) {
            scope.push(op->name, op->value);
        }

        Expr mutated_body = mutate(op->body);

        if (new_value.same_as(op->value) &&
            mutated_body.same_as(op->body)) {
            return op;
        } else if (was_vectorized) {
            scope.pop(op->name);
            for (const auto &widened_var : widened_vars[op->name]) {
                mutated_body = Let::make(widened_var.first, widened_var.second, mutated_body);
            }

            return mutated_body;
        } else {
            return Let::make(op->name, new_value, mutated_body);
        }
    }

    Stmt visit(const LetStmt *op) override {
        Expr new_value = mutate(op->value);

        // Check if the value was vectorized by this mutator.
        bool was_vectorized = (!op->value.type().is_vector() &&
                               new_value.type().is_vector());

        if (was_vectorized) {
            scope.push(op->name, op->value);
            // Also keep track of the original let, in case inner code scalarizes.
            containing_lets.emplace_back(op->name, op->value);
        }

        Stmt mutated_body = mutate(op->body);

        if (was_vectorized) {
            containing_lets.pop_back();
            scope.pop(op->name);

            for (const auto &widened_var : widened_vars[op->name]) {
                const string &widened_name = widened_var.first;
                const Expr &widened_value = widened_var.second;

                // Inner code might have extracted my lanes using
                // extract_lane, which introduces a shuffle_vector. If
                // so we should define separate lets for the lanes and
                // get it to use those instead.
                mutated_body = ReplaceShuffleVectors(widened_name).mutate(mutated_body);

                // Check if inner code wants my individual lanes.
                Type t = widened_value.type();
                for (int i = 0; i < t.lanes(); i++) {
                    string lane_name = widened_name + ".lane." + std::to_string(i);
                    if (stmt_uses_var(mutated_body, lane_name)) {
                        mutated_body =
                            LetStmt::make(lane_name, extract_lane(widened_value, i), mutated_body);
                    }
                }

                // Inner code may also have wanted my max or min lane
                bool uses_min_lane = stmt_uses_var(mutated_body, widened_name + ".min_lane");
                bool uses_max_lane = stmt_uses_var(mutated_body, widened_name + ".max_lane");

                if (uses_min_lane || uses_max_lane) {
                    Interval i = bounds_of_lanes(widened_value);

                    if (uses_min_lane) {
                        mutated_body =
                            LetStmt::make(widened_name + ".min_lane", i.min, mutated_body);
                    }

                    if (uses_max_lane) {
                        mutated_body =
                            LetStmt::make(widened_name + ".max_lane", i.max, mutated_body);
                    }
                }

                mutated_body = LetStmt::make(widened_name, widened_value, mutated_body);
            }
        }

        if (new_value.same_as(op->value) &&
            mutated_body.same_as(op->body)) {
            return op;
        } else if (was_vectorized) {
            return mutated_body;
        } else {
            return LetStmt::make(op->name, new_value, mutated_body);
        }
    }

    Stmt visit(const Provide *op) override {
        vector<Expr> new_args(op->args.size());
        vector<Expr> new_values(op->values.size());
        bool changed = false;

        // Mutate the args
        int max_lanes = 0;
        for (size_t i = 0; i < op->args.size(); i++) {
            Expr old_arg = op->args[i];
            Expr new_arg = mutate(old_arg);
            if (!new_arg.same_as(old_arg)) changed = true;
            new_args[i] = new_arg;
            max_lanes = std::max(new_arg.type().lanes(), max_lanes);
        }

        for (size_t i = 0; i < op->args.size(); i++) {
            Expr old_value = op->values[i];
            Expr new_value = mutate(old_value);
            if (!new_value.same_as(old_value)) changed = true;
            new_values[i] = new_value;
            max_lanes = std::max(new_value.type().lanes(), max_lanes);
        }

        if (!changed) {
            return op;
        } else {
            // Widen the args to have the same lanes as the max lanes found
            for (size_t i = 0; i < new_args.size(); i++) {
                new_args[i] = widen(new_args[i], max_lanes);
            }
            for (size_t i = 0; i < new_values.size(); i++) {
                new_values[i] = widen(new_values[i], max_lanes);
            }
            return Provide::make(op->name, new_values, new_args);
        }
    }

    Stmt visit(const Store *op) override {
        Expr predicate = mutate(op->predicate);
        Expr value = mutate(op->value);
        Expr index = mutate(op->index);

        if (predicate.same_as(op->predicate) && value.same_as(op->value) && index.same_as(op->index)) {
            return op;
        } else {
            int lanes = std::max(predicate.type().lanes(), std::max(value.type().lanes(), index.type().lanes()));
            return Store::make(op->name, widen(value, lanes), widen(index, lanes),
                               op->param, widen(predicate, lanes), op->alignment);
        }
    }

    Stmt visit(const AssertStmt *op) override {
        return (op->condition.type().lanes() > 1) ? scalarize(op) : op;
    }

    Stmt visit(const IfThenElse *op) override {
        Expr cond = mutate(op->condition);
        int lanes = cond.type().lanes();

        debug(3) << "Vectorizing \n"
                 << "Old: " << op->condition << "\n"
                 << "New: " << cond << "\n";

        Stmt then_case = mutate(op->then_case);
        Stmt else_case = mutate(op->else_case);

        if (lanes > 1) {
            // We have an if statement with a vector condition,
            // which would mean control flow divergence within the
            // SIMD lanes.

            bool vectorize_predicate = !(uses_gpu_vars(cond) || (vectorized_vars.size() > 1));

            Stmt predicated_stmt;
            if (vectorize_predicate) {
                PredicateLoadStore p(vectorized_vars.front().name, cond, in_hexagon, target);
                predicated_stmt = p.mutate(then_case);
                vectorize_predicate = p.is_vectorized();
            }
            if (vectorize_predicate && else_case.defined()) {
                PredicateLoadStore p(vectorized_vars.front().name, !cond, in_hexagon, target);
                predicated_stmt = Block::make(predicated_stmt, p.mutate(else_case));
                vectorize_predicate = p.is_vectorized();
            }

            debug(4) << "IfThenElse should vectorize predicate "
                     << "? " << vectorize_predicate << "; cond: " << cond << "\n";
            debug(4) << "Predicated stmt:\n"
                     << predicated_stmt << "\n";

            // First check if the condition is marked as likely.
            const Call *c = cond.as<Call>();
            if (c && (c->is_intrinsic(Call::likely) ||
                      c->is_intrinsic(Call::likely_if_innermost))) {

                // The meaning of the likely intrinsic is that
                // Halide should optimize for the case in which
                // *every* likely value is true. We can do that by
                // generating a scalar condition that checks if
                // the least-true lane is true.
                Expr all_true = bounds_of_lanes(c->args[0]).min;
                // Wrap it in the same flavor of likely
                all_true = Call::make(Bool(), c->name,
                                      {all_true}, Call::PureIntrinsic);

                if (!vectorize_predicate) {
                    // We should strip the likelies from the case
                    // that's going to scalarize, because it's no
                    // longer likely.
                    Stmt without_likelies =
                        IfThenElse::make(op->condition.as<Call>()->args[0],
                                         op->then_case, op->else_case);
                    Stmt stmt =
                        IfThenElse::make(all_true,
                                         then_case,
                                         scalarize(without_likelies));
                    debug(4) << "...With all_true likely: \n"
                             << stmt << "\n";
                    return stmt;
                } else {
                    Stmt stmt =
                        IfThenElse::make(all_true,
                                         then_case,
                                         predicated_stmt);
                    debug(4) << "...Predicated IfThenElse: \n"
                             << stmt << "\n";
                    return stmt;
                }
            } else {
                // It's some arbitrary vector condition.
                if (!vectorize_predicate) {
                    debug(4) << "...Scalarizing vector predicate: \n"
                             << Stmt(op) << "\n";
                    return scalarize(op);
                } else {
                    Stmt stmt = predicated_stmt;
                    debug(4) << "...Predicated IfThenElse: \n"
                             << stmt << "\n";
                    return stmt;
                }
            }
        } else {
            // It's an if statement on a scalar, we're ok to vectorize the innards.
            debug(3) << "Not scalarizing if then else\n";
            if (cond.same_as(op->condition) &&
                then_case.same_as(op->then_case) &&
                else_case.same_as(op->else_case)) {
                return op;
            } else {
                return IfThenElse::make(cond, then_case, else_case);
            }
        }
    }

    Stmt visit(const For *op) override {
        ForType for_type = op->for_type;

        Expr min = mutate(op->min);
        Expr extent = mutate(op->extent);

        Stmt body = op->body;

        if (min.type().is_vector()) {
            // Rebase the loop to zero and try again
            Expr var = Variable::make(Int(32), op->name);
            Stmt body = substitute(op->name, var + op->min, op->body);
            Stmt transformed = For::make(op->name, 0, op->extent, for_type, op->device_api, body);
            return mutate(transformed);
        }

        if (extent.type().is_vector()) {
            // We'll iterate up to the max over the lanes, but
            // inject an if statement inside the loop that stops
            // each lane from going too far.

            extent = bounds_of_lanes(extent).max;
            Expr var = Variable::make(Int(32), op->name);
            body = IfThenElse::make(likely(var < op->min + op->extent), body);
        }

        if (op->for_type == ForType::Vectorized) {
            const IntImm *extent_int = extent.as<IntImm>();
            if (!extent_int || extent_int->value <= 1) {
                user_error << "Loop over " << op->name
                           << " has extent " << extent
                           << ". Can only vectorize loops over a "
                           << "constant extent > 1\n";
            }

            vectorized_vars.push_back({op->name, min, (int)extent_int->value});
            update_replacements();
            body = mutate(body);
            vectorized_vars.pop_back();
            update_replacements();
            return body;
        } else {
            body = mutate(body);

            if (min.same_as(op->min) &&
                extent.same_as(op->extent) &&
                body.same_as(op->body) &&
                for_type == op->for_type) {
                return op;
            } else {
                return For::make(op->name, min, extent, for_type, op->device_api, body);
            }
        }
    }

    Stmt visit(const Allocate *op) override {
        std::vector<Expr> new_extents;
        Expr new_expr;

        // The new expanded dimensions are innermost.
        for (const auto &vv : vectorized_vars) {
            new_extents.emplace_back(vv.lanes);
        }

        for (size_t i = 0; i < op->extents.size(); i++) {
            Expr extent = mutate(op->extents[i]);
            // For vector sizes, take the max over the lanes. Note
            // that we haven't changed the strides, which also may
            // vary per lane. This is a bit weird, but the way we
            // set up the vectorized memory means that lanes can't
            // clobber each others' memory, so it doesn't matter.
            if (extent.type().is_vector()) {
                extent = bounds_of_lanes(extent).max;
            }
            new_extents.push_back(extent);
        }

        if (op->new_expr.defined()) {
            new_expr = mutate(op->new_expr);
            user_assert(new_expr.type().is_scalar())
                << "Cannot vectorize an allocation with a varying new_expr per vector lane.\n";
        }

        Stmt body = op->body;

        // Rewrite loads and stores to this allocation like so:
        // foo[x] -> foo[x*lanes + v]
        vector<string> vs;
        for (const auto &vv : vectorized_vars) {
            string v = unique_name('v');
            body = RewriteAccessToVectorAlloc(v, op->name, vv.lanes).mutate(body);
            vs.push_back(v);
            scope.push(v, Variable::make(Int(32), vv.name));
        }

        body = mutate(body);
        for (const auto &v : vs) {
            scope.pop(v);
        }

        // Replace the widened 'v' with the actual ramp
        // foo[x*lanes + widened_v] -> foo[x*lanes + ramp(...)]
        for (size_t ix = 0; ix < vectorized_vars.size(); ix++) {
            for (const auto &widened_var : widened_vars[vs[ix]]) {
                body = substitute(widened_var.first, widened_var.second, body);
            }

            // The variable itself could still exist inside an inner scalarized block.
            body = substitute(vs[ix], Variable::make(Int(32), vectorized_vars[ix].name), body);
        }

        return Allocate::make(op->name, op->type, op->memory_type, new_extents, op->condition, body, new_expr, op->free_function);
    }

    Stmt scalarize(Stmt s) {
        // Wrap a serial loop around it. Maybe LLVM will have
        // better luck vectorizing it.

        s = SerializeLoops().mutate(s);
        // We'll need the original scalar versions of any containing lets.
        for (size_t i = containing_lets.size(); i > 0; i--) {
            const auto &l = containing_lets[i - 1];
            s = LetStmt::make(l.first, l.second, s);
        }

        for (int ix = vectorized_vars.size() - 1; ix >= 0; ix--) {
            s = For::make(vectorized_vars[ix].name, vectorized_vars[ix].min,
                          vectorized_vars[ix].lanes, ForType::Serial, DeviceAPI::None, s);
        }

        return s;
    }

    Expr scalarize(Expr e) {
        // This method returns a select tree that produces a vector lanes
        // result expression
        // Q: How do I trigger this?
        user_assert(replacements.size() == 1) << "Cannot scalarize nested vectorization\n";
        string var = replacements.begin()->first;
        Expr replacement = replacements.begin()->second;

        Expr result;
        int lanes = replacement.type().lanes();

        for (int i = lanes - 1; i >= 0; --i) {
            // Hide all the vector let values in scope with a scalar version
            // in the appropriate lane.
            for (Scope<Expr>::const_iterator iter = scope.cbegin(); iter != scope.cend(); ++iter) {
                string name = iter.name() + ".lane." + std::to_string(i);
                Expr lane = extract_lane(iter.value(), i);
                e = substitute(iter.name(), Variable::make(lane.type(), name), e);
            }

            // Replace uses of the vectorized variable with the extracted
            // lane expression
            e = substitute(var, i, e);

            if (i == lanes - 1) {
                result = Broadcast::make(e, lanes);
            } else {
                Expr cond = (replacement == Broadcast::make(i, lanes));
                result = Select::make(cond, Broadcast::make(e, lanes), result);
            }
        }

        debug(0) << e << " -> " << result << "\n";

        return result;
    }

    void update_replacements() {
        replacements.clear();
        replacements_from_zero.clear();

        for (const auto &var : vectorized_vars) {
            replacements[var.name] = var.min;
            replacements_from_zero[var.name] = 0;
        }

        Expr strided_ones = 1;
        for (int ix = vectorized_vars.size() - 1; ix >= 0; ix--) {
            for (int ik = 0; ik < (int)vectorized_vars.size(); ik++) {
                if (ix == ik) {
                    replacements[vectorized_vars[ik].name] =
                        Ramp::make(replacements[vectorized_vars[ik].name],
                                   strided_ones,
                                   vectorized_vars[ix].lanes);
                    replacements_from_zero[vectorized_vars[ik].name] =
                        Ramp::make(replacements_from_zero[vectorized_vars[ik].name],
                                   strided_ones,
                                   vectorized_vars[ix].lanes);
                } else {
                    replacements[vectorized_vars[ik].name] =
                        Broadcast::make(replacements[vectorized_vars[ik].name],
                                        vectorized_vars[ix].lanes);
                    replacements_from_zero[vectorized_vars[ik].name] =
                        Broadcast::make(replacements_from_zero[vectorized_vars[ik].name],
                                        vectorized_vars[ix].lanes);
                }
            }

            strided_ones = Broadcast::make(strided_ones, vectorized_vars[ix].lanes);
        }
    }

public:
    VectorSubs(const std::vector<VectorizedVar> &vv, bool in_hexagon, const Target &t)
        : vectorized_vars(vv), target(t), in_hexagon(in_hexagon) {
        update_replacements();
    }
};

// Vectorize all loops marked as such in a Stmt
class VectorizeLoops : public IRMutator {
    const Target &target;
    bool in_hexagon;

    std::vector<VectorizedVar> vectorized_vars;

    using IRMutator::visit;

    Stmt visit(const For *for_loop) override {
        bool old_in_hexagon = in_hexagon;
        if (for_loop->device_api == DeviceAPI::Hexagon) {
            in_hexagon = true;
        }

        Stmt stmt;
        if (for_loop->for_type == ForType::Vectorized) {
            const IntImm *extent = for_loop->extent.as<IntImm>();
            if (!extent || extent->value <= 1) {
                user_error << "Loop over " << for_loop->name
                           << " has extent " << for_loop->extent
                           << ". Can only vectorize loops over a "
                           << "constant extent > 1\n";
            }

            vectorized_vars.push_back({for_loop->name, for_loop->min, (int)extent->value});
            stmt = VectorSubs(vectorized_vars, in_hexagon, target).mutate(for_loop->body);
            vectorized_vars.clear();
        } else {
            stmt = IRMutator::visit(for_loop);
        }

        if (for_loop->device_api == DeviceAPI::Hexagon) {
            in_hexagon = old_in_hexagon;
        }

        return stmt;
    }

public:
    VectorizeLoops(const Target &t)
        : target(t), in_hexagon(false) {
    }
};

}  // Anonymous namespace

Stmt vectorize_loops(const Stmt &s, const Target &t) {
    return VectorizeLoops(t).mutate(s);
}

}  // namespace Internal
}  // namespace Halide
