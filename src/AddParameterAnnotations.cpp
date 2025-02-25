#include "IRMutator.h"
#include "IROperator.h"
#include "IRVisitor.h"
#include "Simplify.h"
#include "Var.h"
#include "VectorizeLoops.h"

#include "CodeGen_C.h"

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::tuple;
using std::set;
using std::string;
using std::vector;

namespace {

Expr add(const Expr &original, const Expr &added){
    Expr result;
    if(original.defined()){
        result = original && added;
    } else {
        result = added;
    }
    return result;
}

void get_buffer_annotations(const Parameter &buf, vector<Annotation> &res){
    int dim = buf.dimensions();
    Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), buf.name() + ".buffer");
    for(int i=0; i<dim; i++){
        Expr constraint;
        if(buf.min_constraint(i).defined()){
            Expr min_val = Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
            Expr new_constraint = min_val == buf.min_constraint(i);
            constraint = add(constraint, new_constraint);
        }
            
        if(buf.extent_constraint(i).defined()){
            Expr extent_val = Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
            Expr new_constraint = extent_val == buf.extent_constraint(i);
            constraint = add(constraint, new_constraint);
        }

        if(buf.stride_constraint(i).defined()){
            Expr stride_val = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
            Expr new_constraint = stride_val == buf.stride_constraint(i);
            constraint = add(constraint, new_constraint);
        }
        if(constraint.defined()){
            res.emplace_back(AnnExpr::make(AnnotationType::Context, constraint));
        }
    }
}

class UpdateBufferAnnotations: public IRMutator {
    using IRMutator::visit;

    const map<string, vector<tuple<Expr,Expr,Expr>>> &buffer_constraints;

    enum VarType {
        Min,
        Extent,
        Stride
    };

    Expr get_min(string name, int i){
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end() ) 
            << "Pipeline annotation for images contain a call to a non-image type, which is not allowed: " << name;

        Expr e = std::get<0>(constraints->second[i]);
        if(e.defined()) return e;
        
        if(top_level){
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
            return Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
        } else {
            return Variable::make(Int(32), name + ".min." + std::to_string(i));
        }
    }

    Expr get_extent(string name, int i){
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end() )
            << "Pipeline annotation for images contain a call to a non-image type, which is not allowed: " << name;

        Expr e = std::get<1>(constraints->second[i]);
        if(e.defined()) return e;

        if(top_level){
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
            return Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
        } else {
            return Variable::make(Int(32), name + ".extent." + std::to_string(i));
        }
    }

     Expr get_stride(string name, int i){
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end() ) 
            << "Pipeline annotation for images contain a call to a non-image type, which is not allowed: " << name;

        Expr e = std::get<2>(constraints->second[i]);
        if(e.defined()) return e;
        
        if(top_level){
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
            return Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
        } else {
            return Variable::make(Int(32), name + ".stride." + std::to_string(i));
        }
    }

    Expr visit(const Variable *op) override {
        if(!top_level)
            return op;
            
        string name = op->name;
        int num = -1;
        VarType prop;

        if(ends_with(name, ".0")){
            num = 0;
        } else if(ends_with(name, ".1")) {
            num = 1;
        } else if(ends_with(name, ".2")) {
            num = 2;
        } else if(ends_with(name, ".3")) {
            num = 3;
        } else if(ends_with(name, ".4")) {
            num = 4;
        } else if(ends_with(name, ".5")) {
            num = 5;
        } else if(ends_with(name, ".6")) {
            num = 6;
        } else if(ends_with(name, ".7")) {
            num = 7;
        } else if(ends_with(name, ".8")) {
            num = 8;
        } else if(ends_with(name, ".9")) {
            num = 9;
        }

        
        
        if(num >= 0){
            name.erase(name.length()-2);
            if(ends_with(name, ".min")){
                prop = VarType::Min;
                name.erase(name.length()-4);
            } else if(ends_with(name, ".extent")){
                prop = VarType::Extent;
                name.erase(name.length()-7);
            } else if(ends_with(name, ".stride")){
                prop = VarType::Stride;
                name.erase(name.length()-7);
            } else {
                return op;
            }

            if(name != ""){
                switch(prop) {
                    case VarType::Min:
                        return get_min(name, num);
                    case VarType::Extent:
                        return get_extent(name, num);
                    case VarType::Stride:
                        return get_stride(name, num);
                }
            }
        }

        return op;
    }

    Expr visit(const Call *call) override {
        if(call->is_extern() || call->is_intrinsic()){
            return IRMutator::visit(call);
        }
        
        string name = call->name;
        Type type;
        size_t dimensions;
        
        if(call->call_type == Call::CallType::Halide){
            // Ugly, but calls to images get inlined to some sort of function ending on _im, 
            // which are removed elsewhere in the pipeline, but not here yet
            if(ends_with(name, "_im"))
                name.erase(name.length()-3);

            Function f(call->func);
            user_assert(f.outputs() == 1)
                << "Function " << name << "has zero or more than one outputs, which we do not yet support";
            type = f.output_types().front();
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
            << "Annotations for images contain a call to an image with "<< call->args.size() 
            << " arguments, but the dimensionality of: \"" << call->name << "\" is " << dimensions;
        
        vector<Expr> new_args(dimensions);
        for (size_t i = 0; i < dimensions; i++) {
            const Expr &old_arg = call->args[i];
            Expr new_arg = mutate(old_arg);
            new_args[i] = std::move(new_arg);
        }
        
        for(size_t i=0; i < dimensions;i++){
            Expr min = get_min(name, i);
            Expr stride = get_stride(name, i);
            Expr added_dimension = (new_args[i] - min) * stride;
            if(i==0)
                index = added_dimension;
            else
                index = Add::make(index, added_dimension);
        }

        index = mutate(index);

        if(top_level){
            name = name + ".buffer.host";
        }
        return Load::make(type, name, index, Buffer<>(), call->param, const_true(), ModulusRemainder(), Expr());
    }

public:
    UpdateBufferAnnotations(const map<string, vector<tuple<Expr,Expr,Expr>>> &buffer_constraints, bool top_level) 
      : buffer_constraints(buffer_constraints), top_level(top_level) {}

    bool top_level;
};

//class AnnotationMaker{
//public:
//    virtual void update(UpdateBufferAnnotations &uba) { return; };
//
//    virtual Annotation create_annotation(bool is_parallel, Expr &readfactor) const { return AnnExpr::make(); };
//};

//class PermissionMaker : public AnnotationMaker {
//    string name;
//    Type buffer_type;
//
//public:
//    PermissionMaker(string name, Type buffer_type) : name(name), buffer_type(buffer_type) { }
//
//    Annotation create_annotation(bool is_parallel, Expr &readfactor) const override {
//        Expr pred = Predicate::make(name, {}, read(readfactor), {buffer_type}, Predicate::PredicateType::Complete);
//        AnnotationType anntype = is_parallel ? AnnotationType::Context : AnnotationType::LoopInvariant;
//        return AnnExpr::make(anntype, pred);
//    }
//};

class AnnotationMaker {
    Expr condition;

    bool is_perm;
    bool is_pure_eq;

    string name;
    Type buffer_type;
    vector<string> forall_vars;
    Expr bound;
public:
    AnnotationMaker(string name, Type buffer_type, Expr &condition) : condition(condition), is_perm(false), is_pure_eq(false), name(name), buffer_type(buffer_type) { }

    AnnotationMaker(string name, Type buffer_type) : is_perm(true), name(name), buffer_type(buffer_type) { }

    AnnotationMaker(string name, Type buffer_type, vector<string> forall_vars, Expr& bound, Expr &condition) : condition(condition), is_perm(false), is_pure_eq(true),
     name(name), buffer_type(buffer_type), forall_vars(forall_vars), bound(bound)  { }

    Annotation create_annotation(bool is_parallel, Expr &readfactor) const {
        AnnotationType anntype = is_parallel ? AnnotationType::Context : AnnotationType::LoopInvariant;
        if(is_perm || is_pure_eq){
            Expr pred = Predicate::make(name, name, {}, read(readfactor), {buffer_type}, Predicate::PredicateType::Complete);
            if(is_perm){
                return AnnExpr::make(anntype, pred);
            } else {
                Expr f = forall(forall_vars, bound, condition);
                Expr unfold = Call::make(UInt(1), Call::unfolding_in, {pred, f}, Call::PureIntrinsic);
                return AnnExpr::make(anntype, unfold);
            }
        } else {
            Expr pred = Predicate::make(name, name, {}, read(readfactor), {buffer_type}, Predicate::PredicateType::Complete);
            Expr unfold = Call::make(UInt(1), Call::unfolding_in, {pred, condition}, Call::PureIntrinsic);
            return AnnExpr::make(anntype, unfold);
        }
    }

    void update(UpdateBufferAnnotations &uba) {
        condition = simplify(uba.mutate(condition));
        bound = simplify(uba.mutate(bound));
    }
};

vector<Annotation> create_annotations(vector<AnnotationMaker> anns, bool is_parallel, Expr &readfactor){
    vector<Annotation> res;
    for(auto const &a: anns){
        res.emplace_back(a.create_annotation(is_parallel, readfactor));
    }
    return res;
}

class ContainsFunctionCall: public IRVisitor {
public:
    ContainsFunctionCall(string function) : contains_call(false), function(function) { }

    bool contains_call;
private:
    string function;
    
    using IRVisitor::visit;

    void visit(const Load *load) override {
        IRVisitor::visit(load);

        if(load->name == function){
            contains_call = true;
        }
    }

    // Evaluate is to annotate GPU barriers
    void visit(const Evaluate *op) override {
        op->value.accept(this);
        // Gpu barriers may say something about permissions
        for(const Annotation &a : op->annotations){
            a.accept(this);
        }
    }

    void visit(const For *op) override {
        op->min.accept(this);
        op->extent.accept(this);
        op->body.accept(this);
        // We should visit annotations now, since if they refer to a function, the annotation information is needed.
        for(const Annotation &a : op->annotations){
            a.accept(this);
        }
    }
};

class AddParameterAnnotations : public IRMutator {
    map<string,vector<AnnotationMaker>> proven_annotations;
    map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints;

    vector<Expr> parallel_read_factor;

    using IRMutator::visit;

    Stmt visit(const For *for_loop) override {
        if(for_loop->is_parallel()){
            parallel_read_factor.push_back(for_loop->extent);
        }
        Expr currentFactor = make_const(Int(32), 2);
        for(auto f : parallel_read_factor){
            currentFactor = currentFactor * f;
        }

        vector<Annotation> loop_annotations;
        for(auto &it: proven_annotations){
            string name = it.first;
            ContainsFunctionCall cfc(name);
            for_loop->accept(&cfc);
            if(cfc.contains_call){
                vector<Annotation> new_annotations = create_annotations(it.second, for_loop->is_parallel(), currentFactor);
                loop_annotations.insert(loop_annotations.end(), new_annotations.begin(), new_annotations.end());
            }
        }

        loop_annotations.insert(loop_annotations.end(), for_loop->annotations.begin(), for_loop->annotations.end() );

        Stmt body = mutate(for_loop->body);

        if(for_loop->is_parallel()){
            parallel_read_factor.pop_back();
        }

        return For::make(for_loop->name,
                             for_loop->min,
                             for_loop->extent,
                             for_loop->for_type,
                             for_loop->device_api,
                             body,
                             loop_annotations);
    }

    struct BufferInfo {
        Expr bound;
        Expr index;
        vector<Expr> pred_args;
        vector<string> forall_vars;
    };

    BufferInfo process_dimensions(const Parameter &par){
        vector<Expr> pred_args;
        vector<string> forall_vars;
        Expr bound;
        vector<tuple<Expr,Expr,Expr>> buffer_dims;
        Expr index;
        Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), par.name() + ".buffer");
        // Expr buffer_in = Variable::make(par.type(), par.name());
        Expr host = Call::make(Handle(), Call::buffer_get_host, {buffer}, Call::Extern);
        for (int i = 0; i < par.dimensions(); i++) {
            Expr min = par.min_constraint(i);
            Expr extent = par.extent_constraint(i);
            Expr stride = par.stride_constraint(i);
            buffer_dims.emplace_back(min, extent, stride);
            if(!min.defined()){
                internal_error << "Buffers need to have constraints for HaliVer to work";
                min = Variable::make(Int(32), par.name() + ".min." + std::to_string(i));
            }
            if(!extent.defined()){
                internal_error << "Buffers need to have constraints for HaliVer to work";
                extent = Variable::make(Int(32), par.name() + ".extent." + std::to_string(i));
            }
            if(!stride.defined()){
                internal_error << "Buffers need to have stride constraints for HaliVer to work";
                stride = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
                stride = Variable::make(Int(32), par.name() + ".stride." + std::to_string(i));
            }
            
            Var var = Var::implicit(i);
            pred_args.emplace_back(var);
            pred_args.emplace_back(min);
            pred_args.emplace_back(extent);
            forall_vars.emplace_back(var.name());
            Expr new_bound = min <= var && var < min + extent;
            Expr new_index = (var - min) * stride;
            if(i==0){
                bound = new_bound;
                index = new_index;
            } else {
                bound = bound && new_bound;
                index = index + new_index;
            }
        }

        buffer_constraints[par.name()] = buffer_dims;

        BufferInfo result;
        result.bound = bound;
        result.index = index;
        result.pred_args = pred_args;
        result.forall_vars = forall_vars;

        return result;
    }

    void get_output_annotations(const Function &f){
        Parameter dim_0 = f.output_buffers()[0];

        get_buffer_annotations(dim_0, top_level);
        BufferInfo info = process_dimensions(dim_0);

        // Expr load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(), par,const_true(),ModulusRemainder(), Expr());
        // top_level.emplace_back(Permission::make(AnnotationType::Context, info.bound, load, Frac::make(1,1), info.forall_vars));
        for(int idx=0; idx<f.outputs(); idx++){
            Parameter par = f.output_buffers()[idx];
            Expr pred = Predicate::make(par.name(), par.name() + ".buffer.host", info.pred_args, write(), {par.type()}, Predicate::PredicateType::Partial);
            pred = Call::make(pred.type(), Call::trigger, {pred}, Call::PureIntrinsic);
            Annotation pred_forall = AnnExpr::make(AnnotationType::Context, forall(info.forall_vars, info.bound, pred));
            top_level.emplace_back(pred_forall);

            for(const auto& ann :par.annotations()){
                const auto *ann_expr = ann.as<AnnExpr>();
                user_assert(ann_expr) << "No permission annotations allowed";
                user_assert(ann_expr->ann_type == AnnotationType::Ensure) << "Only ensure annotations allowed concerning top level";
                top_level.emplace_back(AnnExpr::make(AnnotationType::Ensure, forall(info.forall_vars, info.bound, ann_expr->condition)));
            }
        }
    }

    void get_input_annotations(const Parameter &par){
        if(!par.is_buffer())
            return;
    
        get_buffer_annotations(par, top_level);
        BufferInfo info = process_dimensions(par);

        // Expr call = Call::make(par, forall_vars_expr);
        Expr load = Load::make(par.type(), par.name(), info.index, Buffer<>(), par, const_true(), ModulusRemainder(), Expr());
        Expr pure_call = Call::make(par.type(), "pure_" + par.name(), {info.index}, Call::Extern);
        AnnotationMaker pmaker(par.name(), par.type());
        proven_annotations[par.name()].emplace_back(pmaker);
        //Expr is_pure = forall(info.forall_vars, info.bound, load == pure_call);
        //AnnotationMaker cmaker(is_pure);
        //AnnotationMaker(string name, Type buffer_type, vector<string> forall_vars, Expr& bound, Expr &condition)
        Expr condition = load == pure_call;
        AnnotationMaker cmaker(par.name(), par.type(), info.forall_vars, info.bound, condition);
        if(false){
            proven_annotations[par.name()].emplace_back(cmaker);
        }

        Expr pred_top_level = Predicate::make(par.name()+".buffer.host", par.name()+".buffer.host" , {}, read(IntImm::make(Int(32), 2)), {par.type()}, Predicate::PredicateType::Complete);
        load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(), par,const_true(), ModulusRemainder(), Expr());
        Expr pure_eq = forall(info.forall_vars, info.bound, load == pure_call);
        pure_eq = Call::make(UInt(1), Call::unfolding_in, {pred_top_level, pure_eq}, Call::PureIntrinsic);
        
        // top_level.emplace_back(Permission::make(AnnotationType::Context, info.bound, load, Frac::make(make_const(Int(32), 1), make_const(Int(32), 2)), info.forall_vars));
        top_level.emplace_back(AnnExpr::make(AnnotationType::Context, pred_top_level));
        if(false){
            top_level.emplace_back(AnnExpr::make(AnnotationType::Context, pure_eq));
        }

        for(const auto& ann :par.annotations()){
            const auto *ann_expr = ann.as<AnnExpr>();
            user_assert(ann_expr) << "No permission annotations allowed";

            AnnotationType annt = ann_expr->ann_type == AnnotationType::ContextEverywhere ? AnnotationType::Context : ann_expr->ann_type;
            user_assert(ann_expr->ann_type == AnnotationType::Require || ann_expr->ann_type == AnnotationType::Context)
                << "Annotation type should be require or context.";

            Expr condition = forall(info.forall_vars, info.bound, ann_expr->condition);
            proven_annotations[par.name()].emplace_back(AnnotationMaker(par.name(), par.type(), condition));
            Expr cond = ann_expr->condition;
            cond = Call::make(UInt(1), Call::unfolding_in, {pred_top_level, cond}, Call::PureIntrinsic);
            top_level.emplace_back(AnnExpr::make(annt, forall(info.forall_vars, info.bound, cond)));
        }
    }

public:
    vector<Annotation> top_level;

    AddParameterAnnotations(vector<Parameter> input, vector<Function> output, vector<Annotation> pipeline_annotations) {
        for(auto &i: input){
            get_input_annotations(i);
        }

        for(auto &o: output){
            get_output_annotations(o);
        }

        UpdateBufferAnnotations uba(buffer_constraints, true);

        for(size_t i=0; i<top_level.size(); i++)
            top_level[i] = simplify(uba.mutate(top_level[i]));

        for(auto &p: pipeline_annotations){
            top_level.emplace_back(simplify(uba.mutate(p)));
        }

        uba.top_level = false;


        for(auto &it: proven_annotations){
            for(size_t i=0; i<it.second.size(); i++){
                it.second[i].update(uba);
            }
        }
    }
};

class UpdateInputBufferCallsToFunction: public IRMutator {
    set<string> input;
    bool in_annotation;

    using IRMutator::visit;

    Annotation visit(const AnnExpr *ann) override {
        in_annotation = true;
        Annotation result = IRMutator::visit(ann);
        in_annotation = false;
        return result;
    }

    Expr visit(const Load *op) override {
        if(input.find(op->name) != input.end() && in_annotation){
            internal_assert(!op->predicate.defined() || is_const_true(op->predicate)) << "Cannot have a predicate here.";
            Expr index = mutate(op->index);
            return Call::make(op->type, "pure_" + op->name, {index}, Call::Extern);
        }

        return IRMutator::visit(op);
    }

public:
    UpdateInputBufferCallsToFunction(vector<Parameter> inp) : in_annotation(false) {
        for(auto &i: inp){
            if(i.is_buffer()){
                this->input.emplace(i.name());
            }
        }
    }
};

}  // namespace

pair<Stmt, vector<Annotation>> add_pipeline_annotations(const Stmt &stmt, vector<Parameter> input, vector<Function> output, vector<Annotation> pipeline_anns) {
    UpdateInputBufferCallsToFunction uibctf(input);
    Stmt s = uibctf.mutate(stmt);

    AddParameterAnnotations apa(input, output, pipeline_anns);
    s = apa.mutate(s);
    return pair<Stmt, vector<Annotation>>(s, apa.top_level);
}

}  // namespace Internal
}  // namespace Halide
