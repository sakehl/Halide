#include "UnrollLoops.h"
#include "Bounds.h"
#include "CSE.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRVisitor.h"
#include "Simplify.h"
#include "Substitute.h"

using std::pair;
using std::tuple;
using std::vector;

namespace Halide {
namespace Internal {

namespace {

class GatherVars : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Variable *var) override {
        vars.emplace_back(var->name);
    }

public:
    vector<std::string> vars;
    GatherVars() = default;
};

class UnrollLoops : public IRMutator {
    using IRMutator::visit;

    vector<pair<std::string, Expr>> lets;
    vector<tuple<std::string, Expr, int64_t, bool>> unrolled_vars;

    Stmt visit(const LetStmt *op) override {
        if (is_pure(op->value)) {
            lets.emplace_back(op->name, op->value);
            Stmt s = IRMutator::visit(op);
            lets.pop_back();
            return s;
        } else {
            return IRMutator::visit(op);
        }
    }

    Stmt visit(const For *for_loop) override {
        if (for_loop->for_type == ForType::Unrolled) {
            // Give it one last chance to simplify to an int
            Expr extent = simplify(for_loop->extent);
            Stmt body = for_loop->body;
            const IntImm *e = extent.as<IntImm>();

            if (e == nullptr) {
                // We're about to hard fail. Get really aggressive
                // with the simplifier.
                for (auto it = lets.rbegin(); it != lets.rend(); it++) {
                    extent = Let::make(it->first, it->second, extent);
                }
                extent = remove_likelies(extent);
                extent = substitute_in_all_lets(extent);
                extent = simplify(extent);
                e = extent.as<IntImm>();
            }

            Expr extent_upper;
            bool use_guard = false;
            if (e == nullptr) {
                // Still no luck. Try taking an upper bound and
                // injecting an if statement around the body.
                extent_upper = find_constant_bound(extent, Direction::Upper, Scope<Interval>());
                if (extent_upper.defined()) {
                    e = extent_upper.as<IntImm>();
                    use_guard = true;
                }
            }

            if (e == nullptr && permit_failed_unroll) {
                // Still no luck, but we're allowed to fail. Rewrite
                // to a serial loop.
                user_warning << "HL_PERMIT_FAILED_UNROLL is allowing us to unroll a non-constant loop into a serial loop. Did you mean to do this?\n";
                body = mutate(body);
                return For::make(for_loop->name, for_loop->min, for_loop->extent,
                                 ForType::Serial, for_loop->device_api, std::move(body), for_loop->annotations);
            }

            user_assert(e)
                << "Can only unroll for loops over a constant extent.\n"
                << "Loop over " << for_loop->name << " has extent " << extent << ".\n";
            body = mutate(body);

            if (e->value == 1) {
                user_warning << "Warning: Unrolling a for loop of extent 1: " << for_loop->name << "\n";
            }
            unrolled_vars.emplace_back(for_loop->name, for_loop->min, e->value, use_guard);
            Stmt iters;
            for (int i = e->value - 1; i >= 0; i--) {
                Stmt iter = substitute(for_loop->name, for_loop->min + i, body);
                if (!iters.defined()) {
                    iters = iter;
                } else {
                    iters = Block::make(iter, iters);
                }
                if (use_guard) {
                    iters = IfThenElse::make(likely_if_innermost(i < for_loop->extent), iters);
                }
            }

            return iters;

        } else {
            Stmt mutated_stmt = IRMutator::visit(for_loop);

            // Although the current for loop was not unrolled, we need to unroll the annotations
            // that were present in the for loop.

            const For *new_for_loop = mutated_stmt.as<For>();

            internal_assert(new_for_loop) << "The outer loop should not have been mutated.";

            // Keep track if any annotations where changed
            bool changed = false;

            vector<Annotation> current_annotations = new_for_loop->annotations;

            for (auto const &unrolled_var : unrolled_vars) {
                std::string name;
                Expr min;
                int64_t extent;
                bool use_guard;
                std::tie(name, min, extent, use_guard) = unrolled_var;
                vector<Annotation> new_annotations;

                for (auto const &ann : current_annotations) {
                    GatherVars gather;
                    ann.accept(&gather);
                    if (std::find(gather.vars.begin(), gather.vars.end(), name) == gather.vars.end()) {
                        // Variable not found, continue
                        new_annotations.emplace_back(ann);
                        continue;
                    }

                    changed = true;
                    // Variable found in annotation, we will unroll it.
                    for (int i = extent - 1; i >= 0; i--) {
                        Annotation unrolled_ann = substitute(name, min + i, ann);
                        if (use_guard) {
                            unrolled_ann = add_antecedent(likely_if_innermost(i < for_loop->extent), unrolled_ann);
                        }
                        new_annotations.emplace_back(unrolled_ann);
                    }
                }
                //We update the current annotations, such that they can be unrolled for other unrolled variables.
                current_annotations = new_annotations;
            }

            if (!changed) {
                return mutated_stmt;
            }

            return For::make(new_for_loop->name, new_for_loop->min, new_for_loop->extent,
                             new_for_loop->for_type, new_for_loop->device_api, new_for_loop->body,
                             current_annotations);
        }
    }
    bool permit_failed_unroll = false;

public:
    UnrollLoops() {
        // Experimental autoschedulers may want to unroll without
        // being totally confident the loop will indeed turn out
        // to be constant-sized. If this feature continues to be
        // important, we need to expose it in the scheduling
        // language somewhere, but how? For now we do something
        // ugly and expedient.

        // For the tracking issue to fix this, see
        // https://github.com/halide/Halide/issues/3479
        permit_failed_unroll = get_env_variable("HL_PERMIT_FAILED_UNROLL") == "1";
    }
};

}  // namespace

Stmt unroll_loops(const Stmt &s) {
    return UnrollLoops().mutate(s);
}

}  // namespace Internal
}  // namespace Halide
