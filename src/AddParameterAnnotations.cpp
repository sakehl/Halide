#include "IRMutator.h"
#include "IROperator.h"
#include "Simplify.h"
#include "Var.h"
#include "VectorizeLoops.h"



namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::tuple;
using std::string;
using std::vector;

namespace {

void buffer_annotations(const Parameter &buf, vector<Annotation> &res){
    int dim = buf.dimensions();
    Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), buf.name() + ".buffer");
    for(int i=0; i<dim; i++){
        if(buf.min_constraint(i).defined()){
            Expr min_val = Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
            res.emplace_back(AnnExpr::make(AnnotationType::ContextEverywhere, EQ::make(min_val, buf.min_constraint(i))));
        }
            
        if(buf.extent_constraint(i).defined()){
            Expr extent_val = Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
            res.emplace_back(AnnExpr::make(AnnotationType::ContextEverywhere, EQ::make(extent_val, buf.extent_constraint(i))));
        }

        if(buf.stride_constraint(i).defined()){
            Expr stride_val = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
            res.emplace_back(AnnExpr::make(AnnotationType::ContextEverywhere, EQ::make(stride_val, buf.stride_constraint(i))));
        }
    }
}

class UpdateBufferAnnotations: public IRMutator {
    using IRMutator::visit;

    map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints;

    Expr visit(const Call *call) override {
        if(call->is_extern() || call->is_intrinsic()){
            return IRMutator::visit(call);
        }
        
        string name = call->name;
        Type type;
        size_t dimensions;
        
        if(call->call_type == Call::CallType::Halide){
            // Ugly, but calls to images get inlined to some sort of function ending on _im, which are removed elsewhere in the pipeline, but not here yet
            if(ends_with(name, "_im"))
                name.erase(name.length()-3);

            auto constraints = buffer_constraints.find(name);
            user_assert(constraints != buffer_constraints.end()) 
                << "Annotations for images contain a call to a non-image type, which is not allowed: \"" << call->name << "\"";

            Function f(call->func);
            user_assert(f.outputs() == 1)
                << "Function " << name << "has zero or more than one outputs, which we do not yet support";
            type = f.output_types().front();\
            dimensions = f.dimensions();
        } else {
            user_assert(call->call_type == Call::CallType::Image) 
                << "Annotations for images contain a call to a non-image type, which is not allowed: \"" << call->name << "\"";
            type = call->param.type();
            dimensions = call->param.dimensions();
        }

        Expr index;
        // Mutate the args
        user_assert(dimensions == call->args.size()) 
            << "Annotations for images contain a call to an image with "<< call->args.size() << " arguments, but the dimensionality of: \"" << call->name << "\" is " << dimensions;
        
        vector<Expr> new_args(dimensions);
        for (size_t i = 0; i < dimensions; i++) {
            const Expr &old_arg = call->args[i];
            Expr new_arg = mutate(old_arg);
            new_args[i] = std::move(new_arg);
        }
   
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end())
            << "Annotations for buffers contain a call to an image, which is not in the input: \"" << call->name << "\"";

        for(size_t i=0; i < dimensions;i++){
            Expr added_dimension = new_args[i] * std::get<2>(constraints->second[i]);
            if(i==0)
                index = added_dimension;
            else
                index = Add::make(index, added_dimension);
        }

        if(top_level){
            name = name + ".buffer.host";
        }
        return Load::make(type, name, index, Buffer<>(), call->param, const_true(), ModulusRemainder());
    }

public:
    UpdateBufferAnnotations(map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints, bool top_level) : buffer_constraints(buffer_constraints), top_level(top_level) {}

    bool top_level;
};

class AddParameterAnnotations : public IRMutator {
    vector<Annotation> proven_annotations;
    map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints;


    using IRMutator::visit;

    Stmt visit(const For *for_loop) override {
        if(proven_annotations.empty())
            return for_loop;
        
        vector<Annotation> new_annotations = proven_annotations;
        new_annotations.insert(new_annotations.end(), for_loop->annotations.begin(), for_loop->annotations.end() );

        Stmt body = mutate(for_loop->body);

        return For::make(for_loop->name,
                             for_loop->min,
                             for_loop->extent,
                             for_loop->for_type,
                             for_loop->device_api,
                             body,
                             new_annotations);
    }

    struct BufferInfo {
        Expr bound;
        Expr index;
        vector<Expr> forall_vars_expr;
        vector<string> forall_vars;
    };

    BufferInfo process_dimensions(const Parameter &par){
        vector<Expr> forall_vars_expr;
            vector<string> forall_vars;
            Expr bound;
            vector<tuple<Expr,Expr,Expr>> buffer_dims;
            Expr index;
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), par.name() + ".buffer");
            // Expr buffer_in = Variable::make(par.type(), par.name());
            Expr host = Call::make(Handle(), Call::buffer_get_host, {buffer}, Call::Extern);
            for (int i = 0; i < par.dimensions(); i++) {
                Expr min = par.min_constraint(i);
                if(!min.defined()){
                    min = Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
                }

                Expr extent = par.extent_constraint(i);
                if(!extent.defined()){
                    extent = Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
                }

                Expr stride = par.stride_constraint(i);
                if(!stride.defined()){
                    stride = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
                }
                buffer_dims.emplace_back(min, extent, stride);
                
                Var var = Var::implicit(i);
                forall_vars_expr.emplace_back(var);
                forall_vars.emplace_back(var.name());
                Expr new_bound = And::make(
                    LE::make(min, var),
                    LT::make(var, Add::make(min, extent)));
                Expr new_index = Mul::make(var, stride);
                if(i==0){
                    bound = new_bound;
                    index = new_index;
                } else {
                    bound = And::make(bound, new_bound);
                    index = Add::make(new_index, index);
                }
            }

            buffer_constraints[par.name()] = buffer_dims;

            BufferInfo result;
            result.bound = bound;
            result.index = index;
            result.forall_vars_expr = forall_vars_expr;
            result.forall_vars = forall_vars;

            return result;
    }

    void get_output_annotations(const Parameter &par){
        if(par.is_buffer()){
            buffer_annotations(par, top_level);

            BufferInfo info = process_dimensions(par);

            Expr load = Load::make(par.type(), par.name()+".buffer.host", info.index,Buffer<>(), par,const_true(),ModulusRemainder());
            top_level.emplace_back(Permission::make(AnnotationType::Context, info.bound, load, Frac::make(1,1), info.forall_vars));

            for(const auto& ann :par.annotations()){
                if (const auto *ann_expr = ann.as<AnnExpr>()){
                    if(ann_expr->ann_type == AnnotationType::Ensure){
                        top_level.emplace_back(AnnExpr::make(AnnotationType::Ensure, Forall::make(info.forall_vars, info.bound, ann_expr->condition)));
                    }
                }
            }
        }
    }

    void get_input_annotations(const Parameter &par){
        if(par.is_buffer()){
            buffer_annotations(par, top_level);

            BufferInfo info = process_dimensions(par);

            
            
            // Expr call = Call::make(par, forall_vars_expr);
            Expr load = Load::make(par.type(), par.name(), info.index, Buffer<>(), par, const_true(), ModulusRemainder());
            proven_annotations.emplace_back(Permission::make(AnnotationType::LoopInvariant, info.bound, load, ReadPerm::make(), info.forall_vars));

            load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(), par,const_true(), ModulusRemainder());
            top_level.emplace_back(Permission::make(AnnotationType::Context, info.bound, load, ReadPerm::make(), info.forall_vars));

            for(const auto& ann :par.annotations()){
                if (const auto *ann_expr = ann.as<AnnExpr>()){
                    if(ann_expr->ann_type == AnnotationType::Require || ann_expr->ann_type == AnnotationType::Context 
                        || ann_expr->ann_type == AnnotationType::ContextEverywhere){
                        proven_annotations.emplace_back(AnnExpr::make(AnnotationType::LoopInvariant, Forall::make(info.forall_vars, info.bound, ann_expr->condition)));
                        
                        AnnotationType top = ann_expr->ann_type == AnnotationType::ContextEverywhere ? AnnotationType::Context : ann_expr->ann_type;
                        top_level.emplace_back(AnnExpr::make(top, Forall::make(info.forall_vars, info.bound, ann_expr->condition)));
                    }
                }
            }
        }
    }

public:
    vector<Annotation> top_level;

    AddParameterAnnotations(vector<Parameter> input, vector<Parameter> output) {
        for(auto &i: input){
            get_input_annotations(i);
        }

        for(auto &o: output){
            get_output_annotations(o);
        }

        UpdateBufferAnnotations uba(buffer_constraints, true);

        for(size_t i=0; i<top_level.size(); i++)
            top_level[i] = simplify(uba.mutate(top_level[i]));

        uba.top_level = false;

        for(size_t i=0; i<proven_annotations.size(); i++)
            proven_annotations[i] = simplify(uba.mutate(proven_annotations[i]));
    }
};


}  // namespace

pair<Stmt, vector<Annotation>> add_parameter_annotations(const Stmt &stmt, vector<Parameter> input, vector<Parameter> output) {
    // Limit the scope of atomic nodes to just the necessary stuff.
    // TODO: Should this be an earlier pass? It's probably a good idea
    // for non-vectorizing stuff too.
    AddParameterAnnotations apa(input, output);
    Stmt s = apa.mutate(stmt);
    return pair<Stmt, vector<Annotation>>(s, apa.top_level);
}

}  // namespace Internal
}  // namespace Halide
