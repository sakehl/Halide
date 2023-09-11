#include "IRMutator.h"
#include "IROperator.h"
#include "IRVisitor.h"
#include "Simplify.h"
#include "Var.h"
#include "VectorizeLoops.h"



namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::tuple;
using std::set;
using std::string;
using std::vector;

namespace {

void get_buffer_annotations(const Parameter &buf, vector<Annotation> &res){
    int dim = buf.dimensions();
    Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), buf.name() + ".buffer");
    for(int i=0; i<dim; i++){
        Expr constraint;
        if(buf.min_constraint(i).defined()){
            Expr min_val = Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
            Expr new_constraint = min_val == buf.min_constraint(i);
            if(constraint.defined()){
                constraint = constraint && new_constraint;
            } else {
                constraint = new_constraint;
            }
        }
            
        if(buf.extent_constraint(i).defined()){
            Expr extent_val = Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
            Expr new_constraint = extent_val == buf.extent_constraint(i);
            if(constraint.defined()){
                constraint = constraint && new_constraint;
            } else {
                constraint = new_constraint;
            }
        }

        if(buf.stride_constraint(i).defined()){
            Expr stride_val = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
            Expr new_constraint = stride_val == buf.stride_constraint(i);
            if(constraint.defined()){
                constraint = constraint && new_constraint;
            } else {
                constraint = new_constraint;
            }
        }
        if(constraint.defined()){
            res.emplace_back(AnnExpr::make(AnnotationType::Context, constraint));
        }
    }
}

class UpdateBufferAnnotations: public IRMutator {
    using IRMutator::visit;

    map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints;

    Expr visit(const Variable *op) override {
        if(!top_level)
            return op;
            
        string name = op->name;
        int num = -1;
        string prop = "";

        if(ends_with(name, ".0")){
            num = 0;
        } else if(ends_with(name, ".1")) {
            num = 1;
        } else if(ends_with(name, ".2")) {
            num = 2;
        }
        
        if(num >= 0){
            name.erase(name.length()-2);
            if(ends_with(name, ".min")){
                prop = Call::buffer_get_min;
                name.erase(name.length()-4);
            } else if(ends_with(name, ".extent")){
                prop = Call::buffer_get_extent;
                name.erase(name.length()-7);
            } else if(ends_with(name, ".stride")){
                prop = Call::buffer_get_stride;
                name.erase(name.length()-7);
            }
            if(name != ""){
                Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
                return Call::make(Int(32), prop,{buffer, num}, Call::Extern);
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

            auto constraints = buffer_constraints.find(name);
            user_assert(constraints != buffer_constraints.end()) 
                << "Annotations for images contain a call to a non-image type, which is not allowed: \"" << call->name << "\": " << name;

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
   
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end())
            << "Annotations for buffers contain a call to an image, which is not in the input: \"" << call->name << "\"";

        for(size_t i=0; i < dimensions;i++){
            Expr min = std::get<0>(constraints->second[i]);
            Expr extent = std::get<2>(constraints->second[i]);
            Expr added_dimension = (new_args[i] - min) * extent;
            if(i==0)
                index = added_dimension;
            else
                index = Add::make(index, added_dimension);
        }

        index = mutate(index);

        if(top_level){
            name = name + ".buffer.host";
        }
        return Load::make(type, name, index, Buffer<>(), call->param, const_true(), ModulusRemainder());
    }

public:
    UpdateBufferAnnotations(map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints, bool top_level) 
      : buffer_constraints(buffer_constraints), top_level(top_level) {}

    bool top_level;
};

class AnnotationMaker{
    AnnotationType originalType;
    bool is_permission;
    tuple<Expr, Expr, Expr, vector<string>> perm;
    Expr condition;
public:
    AnnotationMaker(AnnotationType originalType, Expr bound, Expr location, Expr permis, vector<string> forall) :
        originalType(originalType), is_permission(true), perm(tie(bound, location, permis, forall)) { }

    AnnotationMaker(AnnotationType originalType, Expr condition) :
        originalType(originalType), is_permission(false), condition(condition) { }

    Annotation create_annotation(bool is_parallel) const {
        AnnotationType anntype = is_parallel ? originalType : AnnotationType::LoopInvariant;

        if(is_permission){
            return Permission::make(anntype, std::get<0>(perm), std::get<1>(perm), std::get<2>(perm), std::get<3>(perm));
        } else {
            return AnnExpr::make(anntype, condition);
        }
    }

    void update(UpdateBufferAnnotations &uba){
        if(is_permission){
            Expr bound = simplify(uba.mutate(std::get<0>(perm)));
            Expr location = simplify(uba.mutate(std::get<1>(perm)));
            Expr permis = simplify(uba.mutate(std::get<2>(perm)));
            perm = tie(bound, location, permis, std::get<3>(perm));
        }
        else {
            condition = simplify(uba.mutate(condition));
        }
    }
};

vector<Annotation> create_annotations(vector<AnnotationMaker> anns, bool is_parallel){
    vector<Annotation> res;
    for(const auto &a: anns){
        res.emplace_back(a.create_annotation(is_parallel));
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


    using IRMutator::visit;

    Stmt visit(const For *for_loop) override {

        vector<Annotation> loop_annotations;
        for(auto &it: proven_annotations){
            string name = it.first;
            ContainsFunctionCall cfc(name);
            for_loop->accept(&cfc);
            if(cfc.contains_call){
                vector<Annotation> new_annotations = create_annotations(it.second, for_loop->is_parallel());
                loop_annotations.insert(loop_annotations.end(), new_annotations.begin(), new_annotations.end());
            }
        }

        loop_annotations.insert(loop_annotations.end(), for_loop->annotations.begin(), for_loop->annotations.end() );

        Stmt body = mutate(for_loop->body);

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
            result.forall_vars_expr = forall_vars_expr;
            result.forall_vars = forall_vars;

            return result;
    }

    void get_output_annotations(const Parameter &par){
        if(!par.is_buffer()){
            return;
        }

        get_buffer_annotations(par, top_level);
        BufferInfo info = process_dimensions(par);

        Expr load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(), par,const_true(),ModulusRemainder());
        top_level.emplace_back(Permission::make(AnnotationType::Context, info.bound, load, Frac::make(1,1), info.forall_vars));

        for(const auto& ann :par.annotations()){
            const auto *ann_expr = ann.as<AnnExpr>();
            user_assert(ann_expr) << "No permission annotations allowed";
            user_assert(ann_expr->ann_type == AnnotationType::Ensure) << "Only ensure annotations allowed concerning top level";
            top_level.emplace_back(AnnExpr::make(AnnotationType::Ensure, Forall::make(info.forall_vars, info.bound, ann_expr->condition)));
        }
    }

    void get_input_annotations(const Parameter &par){
        if(!par.is_buffer())
            return;
    
        get_buffer_annotations(par, top_level);
        BufferInfo info = process_dimensions(par);

        // Expr call = Call::make(par, forall_vars_expr);
        Expr load = Load::make(par.type(), par.name(), info.index, Buffer<>(), par, const_true(), ModulusRemainder());
        Expr pure_call = Call::make(par.type(), "pure_" + par.name(), {info.index}, Call::Extern);
        proven_annotations[par.name()].emplace_back(AnnotationType::Context, info.bound, load, ReadPerm::make(), info.forall_vars);

        proven_annotations[par.name()].emplace_back(AnnotationType::Context, 
            Forall::make(info.forall_vars, info.bound, load == pure_call));

        load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(), par,const_true(), ModulusRemainder());
        top_level.emplace_back(Permission::make(AnnotationType::Context, info.bound, load, ReadPerm::make(), info.forall_vars));
        top_level.emplace_back(AnnExpr::make(AnnotationType::Context, 
            Forall::make(info.forall_vars, info.bound, load == pure_call)));

        for(const auto& ann :par.annotations()){
            const auto *ann_expr = ann.as<AnnExpr>();
            user_assert(ann_expr) << "No permission annotations allowed";

            AnnotationType annt = ann_expr->ann_type == AnnotationType::ContextEverywhere ? AnnotationType::Context : ann_expr->ann_type;
            user_assert(ann_expr->ann_type == AnnotationType::Require || ann_expr->ann_type == AnnotationType::Context)
                << "Annotation type should be require or context.";

            proven_annotations[par.name()].emplace_back(annt, Forall::make(info.forall_vars, info.bound, ann_expr->condition));
                    
            top_level.emplace_back(AnnExpr::make(annt, Forall::make(info.forall_vars, info.bound, ann_expr->condition)));
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

pair<Stmt, vector<Annotation>> add_parameter_annotations(const Stmt &stmt, vector<Parameter> input, vector<Parameter> output) {
    UpdateInputBufferCallsToFunction uibctf(input);
    Stmt s = uibctf.mutate(stmt);

    AddParameterAnnotations apa(input, output);
    s = apa.mutate(s);
    return pair<Stmt, vector<Annotation>>(s, apa.top_level);
}

}  // namespace Internal
}  // namespace Halide
