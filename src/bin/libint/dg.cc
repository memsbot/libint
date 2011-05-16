
#include <functional>
#include <fstream>
#include <dg.h>
#include <dg.templ.h>
#include <rr.h>
#include <strategy.h>
#include <prefactors.h>
#include <codeblock.h>
#include <default_params.h>
#include <graph_registry.h>
#include <global_macros.h>
#include <extract.h>
#include <algebra.h>
#include <task.h>
#include <context.h>
#include <intset_to_ints.h>
#include <dims.h>

using namespace std;
using namespace libint2;

#define ONLY_CLONE_IF_DIFF 1

//// utils first

namespace {
  void push(DirectedGraph::VPtrAssociativeContainer& vertices, const DirectedGraph::ver_ptr& v) {
    DirectedGraph::key_type vkey = libint2::key(*v);
    vertices.insert(std::make_pair(vkey,v));
  }
  void push(DirectedGraph::VPtrSequenceContainer& vertices, const DirectedGraph::ver_ptr& v) {
    vertices.push_back(v);
  }
}


DirectedGraph::DirectedGraph() :
  stack_(), targets_(), func_names_(),
  first_to_compute_(), registry_(SafePtr<GraphRegistry>(new GraphRegistry)),
  iregistry_(SafePtr<InternalGraphRegistry>(new InternalGraphRegistry))
{
  stack_.clear();
  targets_.clear();
}

DirectedGraph::~DirectedGraph()
{
  reset();
}

void
DirectedGraph::append_target(const SafePtr<DGVertex>& target)
{
  target->make_a_target();
  append_vertex(target);
  push(targets_,target);
}

SafePtr<DGVertex>
DirectedGraph::append_vertex(const SafePtr<DGVertex>& vertex)
{
  // If this vertex is owned by this graph, return it immediately
  if (vertex->dg() == this) {
#if DEBUG
    std::cout << "append_vertex: vertex " << vertex->label() << " is already on" << endl;
#endif
    return vertex;
  }
  const SafePtr<DGVertex>& vcopy_on_graph = add_vertex(vertex);
  // If this is a new vertex -- tell the vertex who its owner is now
  if (vcopy_on_graph == vertex)
    vertex->dg(this);
  return vcopy_on_graph;
}

SafePtr<DGVertex>
DirectedGraph::add_vertex(const SafePtr<DGVertex>& vertex)
{
  SafePtr<DGVertex> vcopy_on_graph = vertex_is_on(vertex);
  if (vcopy_on_graph)
    return vcopy_on_graph;
  else {
    add_new_vertex(vertex);
    return vertex;
  }
}

void
DirectedGraph::add_new_vertex(const SafePtr<DGVertex>& vertex)
{
#if 0
  // Resize if using std::vector
#if !USE_ASSOCCONTAINER_BASED_DIRECTEDGRAPH
  if (num_vertices() == stack_.capacity()) {
    stack_.resize( stack_.capacity() + default_size_ );
#if DEBUG
    cout << "Increased size of DirectedGraph's stack to "
         << stack_.size() << endl;
#endif
  }
#endif
#endif

  char label[80];  sprintf(label,"vertex%d",num_vertices());
  vertex->set_graph_label(label);
  vertex->dg(this);

  push(stack_,vertex);
#if DEBUG
  cout << "add_new_vertex: added vertex " << vertex->description() << endl;
#endif

  return;
}

const SafePtr<DGVertex>&
DirectedGraph::vertex_is_on(const SafePtr<DGVertex>& vertex) const
{
  if (vertex->dg() == this)
    return vertex;

  static SafePtr<DGVertex> null_ptr;
#if USE_ASSOCCONTAINER_BASED_DIRECTEDGRAPH
  typedef vertices::const_iterator citer;
  typedef vertices::value_type value_type;
  key_type vkey = key(*vertex);
  // find the first elemnt with this key and iterate until vertex is found or key changes
  citer vpos = stack_.find(vkey);
  const citer end = stack_.end();
  if (vpos != end) {
    bool can_find = true;
    while(can_find) {
      if (can_find && (vpos->second)->equiv(vertex)) {
#if DEBUG
	std::cout << "vertex_is_on: " << (vpos->second)->label() << std::endl;
#endif
	return vpos->second;
      }
      ++vpos;
      can_find = (vpos->first == vkey) && (vpos != end);
    }
  }
#else
  typedef vertices::const_reverse_iterator criter;
  typedef vertices::reverse_iterator riter;
  const criter rend = stack_.rend();
  for(criter v=stack_.rbegin(); v!=rend; ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if(vertex->equiv(vptr)) {
#if DEBUG
      std::cout << "vertex_is_on: " << (vptr)->label() << std::endl;
#endif
      return vptr;
    }
  }
#endif
#if DEBUG
  std::cout << "vertex_is_on: NOT " << (vertex)->label() << std::endl;
#endif
  return null_ptr;
}

namespace {
  struct __reset_dgvertex {
    void operator()(SafePtr<DGVertex>& v) {
      v->reset();
#if DEBUG
      std::cout << "DirectedGraph::reset: will unregister " << v->label() << std::endl;
#endif
      // remove this vertex from its SingletonManager
      v->unregister();
    }
  };
  struct __reset_safeptr {
    void operator()(SafePtr<DGVertex>& v) {
      v.reset();
    }
  };
}

void
DirectedGraph::del_vertex(vertices::iterator& v)
{
  static __reset_dgvertex rv;
  if (v == stack_.end())
    throw CannotPerformOperation("DirectedGraph::del_vertex() cannot delete vertex");
  ver_ptr& vptr = vertex_ptr(*v);
  // Cannot delete targets. Should I be able to? Probably not
  if (vptr->is_a_target())
    throw CannotPerformOperation("DirectedGraph::del_vertex() cannot delete targets");
  if (vptr->num_exit_arcs() == 0 && vptr->num_entry_arcs() == 0) {
    stack_.erase(v);
    rv(vertex_ptr(*v));
#if DEBUG
    std::cout << "del_vertex: removed " << (vertex_ptr(*v))->label() << std::endl;
#endif
  }
  else
    throw CannotPerformOperation("DirectedGraph::del_vertex() cannot delete vertex");
}

namespace {
  struct __prepare_to_traverse {
    void operator()(SafePtr<DGVertex>& v) {
      v->prepare_to_traverse();
    }
  };
  __prepare_to_traverse __ptt;
};

void
DirectedGraph::prepare_to_traverse()
{
  foreach(__ptt);
}

void
DirectedGraph::traverse()
{
  // Initialization
  prepare_to_traverse();

  // Start at the targets which don't have parents
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if ((vptr)->is_a_target() && (vptr)->num_entry_arcs() == 0) {
      // First, since this target doesn't have parents we can schedule its computation
      schedule_computation(vptr);

      typedef DGVertex::ArcSetType::const_iterator aciter;
      const aciter abegin = (vptr)->first_exit_arc();
      const aciter aend = (vptr)->plast_exit_arc();
      for(aciter a=abegin; a!=aend; ++a) {
        traverse_from(*a);
      }
    }
  }
}

void
DirectedGraph::traverse_from(const SafePtr<DGArc>& arc)
{
  SafePtr<DGVertex> orig = arc->orig();
  SafePtr<DGVertex> dest = arc->dest();
#if DEBUG_TRAVERSAL
  std::cout << "traverse_from: orig = " << orig << " dest = " << dest << endl;
#endif
  // no need to compute if precomputed
  if (dest->precomputed()) {
#if DEBUG_TRAVERSAL
    std::cout << "traverse from: dest is precomputed" << std::endl;
#endif
    return;
  }
  // if has been hit by all parents ...
  const unsigned int num_tags = dest->tag();
#if DEBUG_TRAVERSAL
  std::cout << "traverse from: tagged dest, ntags = " << num_tags << std::endl;
#endif
  const unsigned int num_parents = dest->num_entry_arcs();
  if (num_tags == num_parents) {

    // ... check if it is on a subtree ...
    if (SafePtr<DRTree> stree = dest->subtree()) {
      // ... if yes, schedule only if it is a root of a subtree
      if (stree->root() == dest)
        schedule_computation(dest);
    }
    // else schedule
    else
      schedule_computation(dest);

      typedef DGVertex::ArcSetType::const_iterator aciter;
      const aciter abegin = dest->first_exit_arc();
      const aciter aend = dest->plast_exit_arc();
      for(aciter a=abegin; a!=aend; ++a)
        traverse_from(*a);
  }
}

void
DirectedGraph::schedule_computation(const SafePtr<DGVertex>& vertex)
{
  vertex->set_postcalc(first_to_compute_);
  first_to_compute_ = vertex;
#if DEBUG || DEBUG_TRAVERSAL
  std::cout << "schedule_computation: " << vertex << endl;
  vertex->print(std::cout);
#endif
}


void
DirectedGraph::debug_print_traversal(std::ostream& os) const
{
  SafePtr<DGVertex> current_vertex = first_to_compute_;

  os << "Debug print of traversal order" << endl;

  do {
    current_vertex->print(os);
    current_vertex = current_vertex->postcalc();
  } while (current_vertex != 0);
}

namespace {

  struct __print_vertices_to_dot {
    bool symbols;
    std::ostream& os;
    __print_vertices_to_dot(bool s, std::ostream& o) : symbols(s), os(o) {}
    void operator()(const SafePtr<DGVertex>& v) {
      os << "  " << v->graph_label()
	 << " [ label = \"";
      if (symbols && v->symbol_set())
	os << v->symbol();
      else
	os << v->label();
      os << "\"]" << endl;
    }
  };

  struct __print_arcs_to_dot {
    std::ostream& os;
    __print_arcs_to_dot(std::ostream& o) : os(o) {}
    void operator()(const SafePtr<DGVertex>& v) {
      typedef DGVertex::ArcSetType::const_iterator aciter;
      const aciter abegin = v->first_exit_arc();
      const aciter aend = v->plast_exit_arc();
      for(aciter a=abegin; a!=aend; ++a) {
	SafePtr<DGVertex> dest = (*a)->dest();
	os << "  " << v->graph_label() << " -> "
	   << dest->graph_label() << endl;
      }
    }
  };

}

void
DirectedGraph::print_to_dot(bool symbols, std::ostream& os) const
{
  os << "digraph G {" << endl
     << "  size = \"8,8\"" << endl;

  __print_vertices_to_dot pvtd(symbols,os);
  foreach(pvtd);

  __print_arcs_to_dot patd(os);
  foreach(patd);

  // Print traversal order using dotted lines
  SafePtr<DGVertex> current_vertex = first_to_compute_;
  if (current_vertex != 0) {
    do {
      SafePtr<DGVertex> next = current_vertex->postcalc();
      if (current_vertex && next) {
        os << "  " << current_vertex->graph_label() << " -> "
           << next->graph_label() << " [ style = dotted ]";
      }
      current_vertex = next;
    } while (current_vertex != 0);
  }

  os << "}" << endl;
}

void
DirectedGraph::reset()
{
  // Reset each vertex, releasing all arcs
  __reset_dgvertex rv;
  foreach(rv);
  __reset_safeptr rptr;
  foreach(rptr);

  // if everything went OK then empty out stack_ and targets_
  stack_.clear();
  targets_.clear();
  first_to_compute_.reset();
  func_names_.clear();
}


/// Apply a strategy to all vertices not yet computed (i.e. which do not have exit arcs)
void
DirectedGraph::apply(const SafePtr<Strategy>& strategy,
                     const SafePtr<Tactic>& tactic)
{
  const SafePtr<DirectedGraph> this_ptr = SafePtr_from_this();
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if ((vptr)->num_exit_arcs() != 0 || (vptr)->precomputed() || !(vptr)->need_to_compute())
      continue;

    SafePtr<RecurrenceRelation> rr0 = strategy->optimal_rr(this_ptr,(vptr),tactic);
    if (rr0 == 0)
      continue;

    // add children to the graph
    SafePtr<DGVertex> target = rr0->rr_target();
    const int num_children = rr0->num_children();
    for(int c=0; c<num_children; c++) {
      SafePtr<DGVertex> child = rr0->rr_child(c);
      bool new_vertex = true;
      SafePtr<DGVertex> dgchild = append_vertex(child);
      if (dgchild != child) {
	child = dgchild;
	new_vertex = false;
      }
      SafePtr<DGArc> arc(new DGArcRel<RecurrenceRelation>(target,child,rr0));
      target->add_exit_arc(arc);
      if (new_vertex)
        apply_to(child,strategy,tactic);
    }
  }
}

/// Add vertex to graph and apply a strategy to vertex recursively
void
DirectedGraph::apply_to(const SafePtr<DGVertex>& vertex,
                        const SafePtr<Strategy>& strategy,
                        const SafePtr<Tactic>& tactic)
{
  bool not_yet_computed = !vertex->precomputed() && vertex->need_to_compute() && (vertex->num_exit_arcs() == 0);
  if (!not_yet_computed)
    return;
  SafePtr<RecurrenceRelation> rr0 = strategy->optimal_rr(SafePtr_from_this(),vertex,tactic);
  if (rr0 == 0)
    return;

  SafePtr<DGVertex> target = rr0->rr_target();
  const int num_children = rr0->num_children();
  for(int c=0; c<num_children; c++) {
    SafePtr<DGVertex> child = rr0->rr_child(c);
    bool new_vertex = true;
      SafePtr<DGVertex> dgchild = append_vertex(child);
      if (dgchild != child) {
	child = dgchild;
	new_vertex = false;
      }
    SafePtr<DGArc> arc(new DGArcRel<RecurrenceRelation>(target,child,rr0));
    target->add_exit_arc(arc);
    if (new_vertex)
      apply_to(child,strategy,tactic);
  }
}

// Optimize out simple recurrence relations
void
DirectedGraph::optimize_rr_out()
{
  replace_rr_with_expr();
  // TODO remove_trivial_arithmetics() seems to be broken when working with [Ti,G12], fix!
#if 1
  remove_trivial_arithmetics();
#endif
  handle_trivial_nodes();
  remove_disconnected_vertices();
  find_subtrees();
}

// Replace recurrence relations with expressions
void
DirectedGraph::replace_rr_with_expr()
{
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if ((vptr)->num_exit_arcs()) {
      SafePtr<DGArc> arc0 = *((vptr)->first_exit_arc());
      SafePtr<DGArcRR> arc0_cast = dynamic_pointer_cast<DGArcRR,DGArc>(arc0);
      if (arc0_cast == 0)
        continue;
      SafePtr<RecurrenceRelation> rr = arc0_cast->rr();

      // Optimize if the recurrence relation is simple and the target and
      // children are of the same type
      if (rr->is_simple() && rr->invariant_type()) {

#if DEBUG || DEBUG_RESTRUCTURE
	std::cout << "replace_rr_with_expr: replacing " << rr->label() << endl;
	std::cout << "replace_rr_with_expr:      with " << rr->rr_expr()->description() << endl;
	std::cout << "replace_rr_with_expr: nchildren = " << rr->num_children() << endl;
	for(unsigned int c=0; c<rr->num_children(); ++c) {
	  std::cout << "replace_rr_with_expr: child " << c << " " << rr->rr_child(c)->description() << endl;
	}
#endif

        // Remove arcs connecting this vertex to children
        (vptr)->del_exit_arcs();

        // and instead insert the numerical expression
        SafePtr<RecurrenceRelation::ExprType> rr_expr = rr->rr_expr();
        SafePtr<DGVertex> expr_vertex = static_pointer_cast<RecurrenceRelation::ExprType,DGVertex>(rr_expr);
        expr_vertex = insert_expr_at((vptr),rr_expr);
        SafePtr<DGArc> arc(new DGArcDirect((vptr),expr_vertex));
        (vptr)->add_exit_arc(arc);

      }
    }
  }
}


//
// This function is very tricky at the moment. The operands have to be added before the operator
// such that operands are guaranteed to be on graph before the operator. This way ExprType::equiv
// can simply compare operand pointers and the operator type (very cheap operations compared
// to the fully recursive explicit comparison).
//
SafePtr<DGVertex>
DirectedGraph::insert_expr_at(const SafePtr<DGVertex>& where, const SafePtr<RecurrenceRelation::ExprType>& expr)
{
#if DEBUG
  cout << "insert_expr_at: " << expr->description() << endl;
#endif

  typedef RecurrenceRelation::ExprType ExprType;
  SafePtr<DGVertex> expr_vertex = static_pointer_cast<DGVertex,ExprType>(expr);

  // If the expression is already on then return it
  if (expr->dg() == this)
    return expr_vertex;

  SafePtr<DGVertex> left_oper = expr->left();
  SafePtr<DGVertex> right_oper = expr->right();
  bool new_left =  (left_oper->dg()  != this);       //
  bool new_right = (right_oper->dg() != this);       //
  bool need_to_clone = false;  // clone expression if (parts of) the expression was found on the graph

  // See if left operand is also an operator
  const SafePtr<ExprType> left_cast = dynamic_pointer_cast<ExprType,DGVertex>(left_oper);
  // if yes -- add it to the graph recursively
  if (left_cast) {
    left_oper = insert_expr_at(expr_vertex,left_cast);
  }
  // else add it directly
  else {
    left_oper = append_vertex(left_oper);
  }
#if ONLY_CLONE_IF_DIFF
  if (left_oper != expr->left()) {
#if DEBUG
    std::cout << "insert_expr_at: append(left) != left" << std::endl;
#endif
    need_to_clone = true;
    new_left = false;
  }
#else
#error "ONLY_CLONE_IF_DIFF must be true"
#endif

  // See if right operand is also an operator
  SafePtr<ExprType> right_cast = dynamic_pointer_cast<ExprType,DGVertex>(right_oper);
  // if yes -- add it to the graph recursively
  if (right_cast) {
    right_oper = insert_expr_at(expr_vertex,right_cast);
  }
  // else add it directly
  else {
    right_oper = append_vertex(right_oper);
  }
#if ONLY_CLONE_IF_DIFF
  if (right_oper != expr->right()) {
#if DEBUG
    std::cout << "insert_expr_at: append(right) != right" << std::endl;
#endif
    need_to_clone = true;
    new_right = false;
  }
#else
#error "ONLY_CLONE_IF_DIFF must be true"
#endif

  if (need_to_clone) {
    SafePtr<ExprType> expr_new(new ExprType(expr,left_oper,right_oper));
    expr_vertex = static_pointer_cast<DGVertex,ExprType>(expr_new);
    int nchildren = expr->num_exit_arcs();
#if DEBUG
    cout << "insert_expr_at: cloned AlgebraicOperator with " << expr->num_exit_arcs() << " children" << endl;
    if (nchildren) {
      cout << "Left:  " << expr->left()->description() << endl;
      cout << "Right: " << expr->right()->description() << endl;
    }
#endif
  }

  SafePtr<DGVertex> dgexpr_vertex = expr_vertex;
  if (new_left || new_right)
    add_new_vertex(expr_vertex);
  else {
    const bool do_cse = registry()->do_cse();
    if (do_cse) {
#if DEBUG
      std::cout << "insert_expr_at: appending vertex " << expr_vertex->description() << std::endl;
#endif
      dgexpr_vertex = append_vertex(expr_vertex);
    }
    else {
      add_new_vertex(expr_vertex);
    }
    if (expr_vertex != dgexpr_vertex) {
      if (new_left || new_right) {
        cout << "Problem detected: AlgebraicOperator is found on the stack but one of its operands was new" << endl;
        cout << expr_vertex->description() << endl;
        cout << dgexpr_vertex->description() << endl;
        throw std::runtime_error("DirectedGraph::insert_expr_at() -- vertex is not new but one of the operands is");
      }
    }
    expr_vertex = dgexpr_vertex;
  }
  SafePtr<DGArc> left_arc(new DGArcDirect(expr_vertex,left_oper));
  expr_vertex->add_exit_arc(left_arc);
  SafePtr<DGArc> right_arc(new DGArcDirect(expr_vertex,right_oper));
  expr_vertex->add_exit_arc(right_arc);
#if DEBUG
  cout << "insert_expr_at: added arc between " << where->description() << " and " << expr_vertex->description() << endl;
#endif

  return expr_vertex;
}

// Replace recurrence relations with expressions
void
DirectedGraph::remove_trivial_arithmetics()
{
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    SafePtr< AlgebraicOperator<DGVertex> > oper_cast = dynamic_pointer_cast<AlgebraicOperator<DGVertex>,DGVertex>((vptr));
    if (oper_cast) {

      typedef DGVertex::ArcSetType::const_iterator aciter;
      aciter a = oper_cast->first_exit_arc();
      SafePtr<DGVertex> left = (*a)->dest();  ++a;
      SafePtr<DGVertex> right = (*a)->dest();

      // 1.0 * x = x
      if (left->equiv(prefactors.N_i[1])) {
        const bool success = remove_vertex_at((vptr),right);
#if DEBUG
        if (success)
          cout << "Removed vertex " << (vptr)->description() << endl;
#endif
      }

      // x * 1.0 = x
      if (right->equiv(prefactors.N_i[1])) {
        const bool success = remove_vertex_at((vptr),left);
#if DEBUG
        if (success)
          cout << "Removed vertex " << (vptr)->description() << endl;
#endif
      }

      // NOTE : more cases to come
    }
  }
}

//
// Handles "trivial" nodes. A node is trivial is it satisfies the following conditions:
// 0) not a target
// 1) has only one child
// 2) the exit arc is of a trivial type (DGArvDirect or IntegralSet_to_Integral applied to node of size 1)
//
// By "handling" I mean either removing the node from the graph or making a node refer to another node so that
// no code is generated for it.
//
void
DirectedGraph::handle_trivial_nodes()
{
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    // if this is a target -- cannot remove
    if ((vptr)->is_a_target())
      continue;
    // or if has more than 1 child
    if ((vptr)->num_exit_arcs() != 1)
      continue;
    SafePtr<DGArc> arc = *((vptr)->first_exit_arc());

    // Is the exit arc DGArcDirect?
    {
      SafePtr<DGArcDirect> arc_cast = dynamic_pointer_cast<DGArcDirect,DGArc>(arc);
      if (arc_cast) {
        // remove the vertex, if possible
        remove_vertex_at((vptr),arc->dest());
      }
    }

    // Is the exit arc DGArcRel<IntegralSet_to_Integrals> and (vptr)->size() == 1?
    if ((vptr)->size() == 1) {
      SafePtr<DGArcRR> arc_cast = dynamic_pointer_cast<DGArcRR,DGArc>(arc);
      if (arc_cast) {
        SafePtr<RecurrenceRelation> rr = arc_cast->rr();
        SafePtr<IntegralSet_to_Integrals_base> rr_cast = dynamic_pointer_cast<IntegralSet_to_Integrals_base,RecurrenceRelation>(rr);
        if (rr_cast)
          (vptr)->refer_this_to(arc->dest());
      }
    }

      // NOTE : more cases to come
  }
}


// If v1 and v2 are connected by DGArcDirect and all entry arcs to v1 are of the DGArcDirect type as well,
// this function will reattach all arcs extering v1 to v2 and remove v1 from the graph alltogether.
// return true if successful, false otherwise
bool
DirectedGraph::remove_vertex_at(const SafePtr<DGVertex>& v1, const SafePtr<DGVertex>& v2)
{
#if DEBUG
    cout << "remove_vertex_at: replacing " << v1->description() << " with " << v2->description() << endl;
#endif

  // Collect all entry arcs in a container
  DGVertex::ArcSetType v1_entry;
  typedef DGVertex::ArcSetType::iterator aiter;
  typedef DGVertex::ArcSetType::const_iterator aciter;
  const aciter abegin = v1->first_entry_arc();
  const aciter aend = v1->plast_entry_arc();
  // Verify that all entry arcs are DGArcDirect
  for(aciter a=abegin; a!=aend; ++a) {
    // See if this is a direct arc -- otherwise cannot do this
    SafePtr<DGArc> arc = (*a);
    SafePtr<DGArcDirect> arc_cast = dynamic_pointer_cast<DGArcDirect,DGArc>(arc);
    if (arc_cast == 0)
      return false;
    v1_entry.push_back(*a);
#if DEBUG
    std::cout << "remove_vertex_at: examined v1 entry arc: from " << (*a)->orig()->description() << " to " << (*a)->dest()->description() << std::endl;
#endif
  }

  // Verify that v1 and v2 are connected by an arc and it is the only arc exiting v1
  /*if (v1->num_exit_arcs() != 1 || v1->exit_arc(0)->dest() != v2)
    return false;*/

  // See if this is a direct arc -- otherwise cannot do this
  SafePtr<DGArc> arc = *(v1->first_exit_arc());
  SafePtr<DGArcDirect> arc_cast = dynamic_pointer_cast<DGArcDirect,DGArc>(arc);
  if (arc_cast == 0)
    return false;

  //
  // OK, now do work!
  //

#if DEBUG || DEBUG_RESTRUCTURE
  std::cout << "remove_vertex_at: v1" << endl;  v1->print(std::cout);
  std::cout << "remove_vertex_at: v2" << endl;  v2->print(std::cout);
#endif

  // Reconnect each of v1's entry arcs to v2
  unsigned int c = 0;
  for(aiter i=v1_entry.begin(); i != v1_entry.end(); ++i, ++c) {
    SafePtr<DGVertex> parent = (*i)->orig();
#if DEBUG || DEBUG_RESTRUCTURE
    cout << "remove_vertex_at: replacing arc " << c << " connecting " << parent->description() << " to " << (*i)->dest()->description() << endl;
    cout << "remove_vertex_at: replacing arc " << c << " connecting " << parent << " to " << (*i)->dest() << endl;
#endif
    SafePtr<DGArcDirect> new_arc(new DGArcDirect(parent,v2));
#if DEBUG || DEBUG_RESTRUCTURE
    cout << "remove_vertex_at:      with arc " << " connecting " << parent->description() << " to " << v2->description() << endl;
    cout << "remove_vertex_at:      with arc " << " connecting " << parent << " to " << v2 << endl;
#endif
    parent->replace_exit_arc(*i,new_arc);
#if DEBUG || DEBUG_RESTRUCTURE
    cout << "Replaced arcs: parent " << parent->description() << " now connected to " << new_arc->dest()->description() << endl;
    cout << "                ptr = " << parent << endl;
    const unsigned int nchildren = parent->num_exit_arcs();
    cout << "               parent has " << nchildren << " children" << endl;
    unsigned int i=0;
    for(aciter a = parent->first_exit_arc(); a!=parent->plast_exit_arc(); ++a, ++i) {
      cout << "               child " << i << " " << (*a)->dest()->description() << endl;
      cout << "               child " << i << " ptr = " << (*a)->dest() << endl;
    }
#endif
  }

#if DEBUG || DEBUG_RESTRUCTURE
  std::cout << "remove_vertex_at: v1" << endl;  v1->print(std::cout);
  std::cout << "remove_vertex_at: v2" << endl;  v2->print(std::cout);
#endif

  // and fully disconnect this vertex
  v1->detach();
#if DEBUG || DEBUG_RESTRUCTURE
  std::cout << "remove_vertex_at: detached " << v1->description() << endl;
#endif

  return true;
}

void
DirectedGraph::remove_disconnected_vertices()
{
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if ((vptr)->num_entry_arcs() == 0 && (vptr)->num_exit_arcs() == 0) {
#if DEBUG
      cout << "Trying to erase disconnected vertex " << (vptr)->description() << " num_vertices = " << num_vertices() << endl;
#endif
      iter vprev = v; --vprev;
      try { del_vertex(v); }
      catch (CannotPerformOperation& v) {
#if DEBUG
        cout << "But couldn't!!!" << endl;
#endif
        ++vprev;
        throw v;
      }
      // current vertex was erased, so need to decrease the iterator as well
      v = vprev;
    }
  }
}

// generate_code uses this helper function. It's declared in libint2 namespace because
// it's also used elsewhere
namespace libint2 {
  std::string
  declare_function(const SafePtr<CodeContext>& context, const SafePtr<ImplicitDimensions>& dims,
                   const SafePtr<CodeSymbols>& args, const std::string& tlabel, const std::string& function_descr,
                   std::ostream& decl) {

    std::string function_name = label_to_funcname(function_descr);
    function_name = context->label_to_name(function_name);

    decl << context->code_prefix();
    std::string func_decl;
    std::ostringstream oss;
    oss << context->type_name<void>() << " "
        << function_name << "(" << context->const_modifier()
        << context->inteval_type_name(tlabel) << "* inteval";
    const unsigned int nargs = args->n();
    if (nargs > 0) {
      // first argument is always the target, which is never a const
      oss << ", " << context->type_name<double*>() << " "
          << args->symbol(0);
      for(unsigned int a=1; a<nargs; a++) {
        oss << ", " << context->type_name<const double*>() << " "
            << args->symbol(a);
      }
    }
    if (!dims->high_is_static()) {
      oss << ", " << context->type_name<int>() << " "
          << dims->high()->id();
    }
    if (!dims->low_is_static()) {
      oss << ", " << context->type_name<int>() << " "
          <<dims->low()->id();
    }
    oss << ")";
    func_decl = oss.str();

    decl << func_decl << context->end_of_stat() << endl;
    decl << context->code_postfix();

    return func_decl;
  }
}

//
//
//
void
DirectedGraph::generate_code(const SafePtr<CodeContext>& context, const SafePtr<MemoryManager>& memman,
                             const SafePtr<ImplicitDimensions>& dims, const SafePtr<CodeSymbols>& args,
                             const std::string& label,
                             std::ostream& decl, std::ostream& def)
{
  LibraryTaskManager& taskmgr = LibraryTaskManager::Instance();
  const std::string tlabel = taskmgr.current().label();

  decl << context->std_header();
  std::string comment("This code computes "); comment += label; comment += "\n";
  if (context->comments_on())
    decl << context->comment(comment) << endl;

  const std::string func_decl = declare_function(context,dims,args,tlabel,label,decl);

  //
  // Generate function's definition
  //

  // include standard headers
  def << context->std_header();
  // include declarations for all function calls:
  // 1) update func_names_
  // 2) include their headers into the current definition file
  update_func_names();
  for(FuncNameContainer::const_iterator fn=func_names_.begin(); fn!=func_names_.end(); fn++) {
    string function_name = (*fn).first;
    def << "#include <"
        << context->label_to_name(context->cparams()->api_prefix() + function_name)
        << ".h>" << endl;
  }
  def << endl;

  def << context->code_prefix();
  def << func_decl << context->open_block() << endl;
  def << context->std_function_header();

  context->reset();
  // if we vectorize by-line then all data is allocated on Libint's stack
  if (context->cparams()->vectorize_by_line())
    allocate_mem(memman,dims,0);
  // otherwise only arrays go on Libint's stack (scalars are handled by the compiler)
  else
    allocate_mem(memman,dims,1);
  assign_symbols(context,dims);
  print_def(context,def,dims);
  def << context->close_block() << endl;
  def << context->code_postfix();
}

void
DirectedGraph::allocate_mem(const SafePtr<MemoryManager>& memman,
                            const SafePtr<ImplicitDimensions>& dims,
                            unsigned int min_size_to_alloc)
{
  // NOTE does this belong here?
  // First, reset tag counters
  prepare_to_traverse();

  //
  // If need to accumulate targets, special events must happen here.
  //
  // NOTES on how to handle accumulation
  // 1) if all targets are unrolled then need to identify which integrals are part of target sets and use += instead of =
  // 2) if no targets are unrolled then allocate extra space for the target quartets and, after code has been generated,
  //    accumulate target sets into those
  // 3) if some targets are not unrolled then still need the extra space. The targets which were not unrolled should be handled
  //    as usual, i.e. not accumulated -- accumulation happens at the end
  if (registry()->accumulate_targets()) {
    // need extra buffers for targets if some are unrolled
    const bool need_copies_of_targets = nonunrolled_targets(targets_);
    iregistry()->accumulate_targets_directly(!need_copies_of_targets);

    if (need_copies_of_targets) {

      // compute the aggregate size of all targets and allocate all space at once
      target_citer end = targets_.end();
      size size_of_targets(0);
      for(target_iter t=targets_.begin(); t!=end; ++t) {
	const ver_ptr& tptr = vertex_ptr(*t);
	size_of_targets += (tptr)->size();
      }
      const address targets_buffer = memman->alloc(size_of_targets);
      iregistry()->size_of_target_accum(size_of_targets);

      // make sure that the space is at the beginning of stack
      if (targets_buffer != 0)
	throw ProgrammingError("DirectedGraph::allocate_mem() -- buffer for targets accumulation is not at the beginning of stack, need MemoryManager::alloc_at");

      // allocate every target accumulator manually
      address curr_ptr(0);
      for(target_iter t=targets_.begin(); t!=end; ++t) {
	const ver_ptr& tptr = vertex_ptr(*t);
	target_accums_.push_back(curr_ptr);
	curr_ptr += (tptr)->size();
      }

    } // need copies of targets
  } // need to accumulate targets

  // Second, MUST allocate space for all targets whose symbols are not set explicitly
  // If a symbol is set means the object is not on stack (e.g. if location of target
  // is passed as an argument to set-level function)
  // This code ensures that target quartets are persistent, i.e. never overwritten, and can be accumulated into
  for(target_iter v=targets_.begin(); v!=targets_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if (!(vptr)->symbol_set())
      (vptr)->set_address(memman->alloc((vptr)->size()));
  }

  //
  // How memory management happens:
  // Go through the traversal order and at each step tag every child
  // Once a child receives same number of tags as the number of parents,
  // it can be deallocated
  //
  SafePtr<DGVertex> vertex = first_to_compute_;
  do {
    SafePtr<DGArcRR> arcrr;
    // memory only needs to be managed for some quantities:
    // this conditional decides whether this vertex is on the stack
    if (
        // If symbol is set then the object is not on stack
        !vertex->symbol_set() &&
        // if address is already set, no need to manage
        !vertex->address_set() &&
        // precomputed objects don't go on stack
        !vertex->precomputed() &&
        // manage only if need to compute ..
        vertex->need_to_compute() &&
        // don't put on stack if smaller than min_size_to_alloc
        // two exceptions, however:
        // 1) it's a target
        // 2) it's an unrolled integral set of size 1, whose only member is not a precomputed quantity
        //    typically integral sets of size 1 are precomputed and don't need to be on stack,
        //    however if they are not the integral will need to be stored somewhere and the rule for
        //    assigning code symbols to members of unrolled integral sets requires the integral set
        //    to have an address assigned
        (vertex->size() > min_size_to_alloc ||
         vertex->is_a_target() ||
         (vertex->size() == 1 && vertex->num_exit_arcs() == 1 &&
          ( (arcrr = dynamic_pointer_cast<DGArcRR,DGArc>(*(vertex->first_exit_arc()))) != 0 ?
              dynamic_pointer_cast<IntegralSet_to_Integrals_base,RecurrenceRelation>(arcrr->rr()) != 0 :
              false ) &&
          !(*(vertex->first_exit_arc()))->dest()->precomputed()
         )
        )
       ) {
      MemoryManager::Address addr = memman->alloc(vertex->size());
      vertex->set_address(addr);

      typedef DGVertex::ArcSetType::const_iterator aciter;
      const aciter abegin = vertex->first_exit_arc();
      const aciter aend = vertex->plast_exit_arc();
      // Verify that all entry arcs are DGArcDirect
      for(aciter a=abegin; a!=aend; ++a) {
        SafePtr<DGVertex> child = (*a)->dest();
        const unsigned int ntags = child->tag();
        // Do NOT deallocate if it's a target!
        if (ntags == child->num_entry_arcs() && child->address_set() && !child->is_a_target()) {
          memman->free(child->address());
        }
      }
    }
    vertex = vertex->postcalc();
  } while (vertex != 0);
}

namespace {
  std::string stack_symbol(const SafePtr<CodeContext>& ctext, const DGVertex::Address& address, const DGVertex::Size& size,
                           const std::string& low_rank, const std::string& veclen,
                           const std::string& prefix)
  {
    ostringstream oss;
    std::string stack_address = ctext->stack_address(address);
    oss << prefix << "[((hsi*" << size << "+"
        << stack_address << ")*" << low_rank << "+lsi)*"
        << veclen << "]";
    return oss.str();
  }

  /// Returns a "vector" form of stack symbol, e.g. converts libint->stack[x] to libint->stack[x+vi]
  inline std::string to_vector_symbol(const SafePtr<DGVertex>& v)
  {
    int current_pos = 0;
    std::string symb = v->symbol();
    // replace repeatedly until the string is exhausted
    while(current_pos != std::string::npos) {

      // find "[" first
      const std::string left_braket("[");
      int where = symb.find(left_braket,current_pos);
      current_pos = where;
      // if the prefix indicating a stack symbol found:
      // 1) make sure vi doesn't appear between the brakets
      // 2) replace "]" with "+vi]"
      if (where != std::string::npos) {
        const std::string right_braket("]");
        int where = symb.find(right_braket,current_pos);
        if (where == std::string::npos)
          throw logic_error("to_vector_symbol() -- address is set but no right braket found");

        const std::string forbidden("vi");
        int pos = symb.find(forbidden,current_pos);
        if (pos == std::string::npos || pos > where) {
          const std::string what_to_add("+vi");
          symb.insert(where,what_to_add);
          current_pos = where + 4;
        }
        else {
          current_pos = where + 1;
        }
      }
    } // end of while
    return symb;
  }
};

void
DirectedGraph::assign_symbols(const SafePtr<CodeContext>& context, const SafePtr<ImplicitDimensions>& dims)
{
  std::ostringstream os;
  const std::string null_str("");
  //const std::string stack_name = registry()->stack_name();
  // There must be a compiler/library/bug on OS X? The above fails... Valgrind under Linux shows no memory problems...
  const std::string stack_name("inteval->stack");

  // Generate the label for the rank of the low dimension
  std::string low_rank = dims->low_label();
  std::string veclen = dims->vecdim_label();

  // First, set symbols for all vertices which have address assigned
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if (!(vptr)->symbol_set() && (vptr)->address_set()) {
      (vptr)->set_symbol(stack_symbol(context,(vptr)->address(),(vptr)->size(),low_rank,veclen,stack_name));
    }
  }

  // Second, find all nodes which were unrolled using IntegralSet_to_Integrals:
  // 1) such nodes do not need symbols generated since they never appear in the code expicitly
  // 2) children of such nodes have symbols that depend on the parent's address
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if ((vptr)->num_exit_arcs() == 0)
      continue;
    SafePtr<DGArc> arc = *((vptr)->first_exit_arc());
    SafePtr<DGArcRR> arc_rr = dynamic_pointer_cast<DGArcRR,DGArc>(arc);
    if (arc_rr == 0)
      continue;
    SafePtr<RecurrenceRelation> rr = arc_rr->rr();
    SafePtr<IntegralSet_to_Integrals_base> iset_to_i = dynamic_pointer_cast<IntegralSet_to_Integrals_base,RecurrenceRelation>(rr);
    if (iset_to_i == 0) {
      continue;
    }
    else {
      typedef DGVertex::ArcSetType::const_iterator aciter;
      const aciter abegin = (vptr)->first_exit_arc();
      const aciter aend = (vptr)->plast_exit_arc();
      unsigned int c = 0;
      // Verify that all entry arcs are DGArcDirect
      for(aciter a=abegin; a!=aend; ++a, ++c) {
        SafePtr<DGVertex> child = (*a)->dest();
        // If a child is precomputed and it's parent symbol is not set -- its symbol will be set as usual
        if (!child->precomputed() || (vptr)->symbol_set()) {
          if ((vptr)->address_set()) {
            child->set_symbol(stack_symbol(context,(vptr)->address()+c,(vptr)->size(),low_rank,veclen,stack_name));
          }
          else {
            child->set_symbol(stack_symbol(context,c,(vptr)->size(),low_rank,veclen,(vptr)->symbol()));
          }
        }
      }
      (vptr)->refer_this_to((*((vptr)->first_exit_arc()))->dest());
      (vptr)->reset_symbol();
    }
  }

  // then process all other symbols, EXCEPT operators
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
#if DEBUG
    cout << "Trying to assign symbol to " << (vptr)->description() << endl;
#endif
    if ((vptr)->symbol_set()) {
      continue;
    }

    // test if the vertex is a static quantity, like a constant
    {
      typedef CTimeEntity<double> cdouble;
      SafePtr<cdouble> ptr_cast = dynamic_pointer_cast<cdouble,DGVertex>((vptr));
      if (ptr_cast) {
        (vptr)->set_symbol(ptr_cast->label());
        continue;
      }
    }

    // test if the vertex is precomputed runtime quantity, like a geometric parameter
    if ((vptr)->precomputed()) {
      std::string symbol("inteval->");
      symbol += context->label_to_name((vptr)->label());
      symbol += "[vi]";
      (vptr)->set_symbol(symbol);
      continue;
    }

    // test if the vertex is other runtime quantity
    {
      typedef RTimeEntity<double> cdouble;
      SafePtr<cdouble> ptr_cast = dynamic_pointer_cast<cdouble,DGVertex>((vptr));
      if (ptr_cast) {
        (vptr)->set_symbol(ptr_cast->label());
        continue;
      }
    }

  } // done with everything BUT operators

  // finally, process all operators (start with most recently added vertices since those are
  // much more likely to be on the bottom of the graph).
  typedef vertices::const_reverse_iterator criter;
  typedef vertices::reverse_iterator riter;
  for(riter v=stack_.rbegin(); v!=stack_.rend(); ++v) {
    ver_ptr& vptr = vertex_ptr(*v);
#if DEBUG
    cout << "Trying to assign symbol to operator " << (vptr)->description() << endl;
#endif
    assign_oper_symbol(context,(vptr));
  }

}

void
DirectedGraph::assign_oper_symbol(const SafePtr<CodeContext>& context, SafePtr<DGVertex>& vertex)
{
  // do nothing if the vertex has a symbol or is not an operator
  if (vertex->symbol_set())
    return;

  {
    typedef AlgebraicOperator<DGVertex> oper;
    SafePtr<oper> ptr_cast = dynamic_pointer_cast<oper,DGVertex>(vertex);
    if (ptr_cast) {
      // is it in a subtree?
      const bool on_a_subtree = (vertex->subtree());

      // If no -- it will be an automatic variable
      if (!on_a_subtree)
        vertex->set_symbol(context->unique_name<EntityTypes::FP>());
      // else assign symbols to left and right arguments
      else {
	typedef DGVertex::ArcSetType::const_iterator aciter;
	aciter arc = ptr_cast->first_exit_arc();
        SafePtr<DGVertex> left = (*arc)->dest(); ++arc;
        SafePtr<DGVertex> right = (*arc)->dest();
        assign_oper_symbol(context,left);
        assign_oper_symbol(context,right);

        std::ostringstream oss;
        oss << "( " << left->symbol() << " ) "
            << ptr_cast->label()
            << " ( " << right->symbol() << " )";
        vertex->set_symbol(oss.str());
      }
    }
  }
}


namespace {
  /// returns how many times token appears in str
  unsigned int nfind(const std::string& str, const std::string& token)
  {
    unsigned int nfinds = 0;
    typedef std::string::size_type size_type;
    size_type current_pos = 0;
    while(1) {
      current_pos = str.find(token,current_pos);
      if (current_pos != std::string::npos) {
        ++nfinds;
        ++current_pos;
      }
      else
        return nfinds;
    }
  }

#define DO_NOT_COUNT_DIV 1
  /// Returns the number of FLOPs in an expression
  unsigned int nflops(const std::string& expr)
  {
    static const std::string mul(" * ");
    static const std::string div(" / ");
    static const std::string plus(" + ");
    static const std::string minus(" - ");
    const unsigned int nflops =
      nfind(expr,mul) +
      nfind(expr,plus) +
      nfind(expr,minus)
#if !DO_NOT_COUNT_DIV
      + nfind(expr,div)
#endif
      ;
    return nflops;
  }
}

namespace {
  template <class Container>
  SafePtr<MemBlockSet>
  to_memoryblks(Container& vertices) {
    SafePtr<MemBlockSet> result(new MemBlockSet);
    typedef typename Container::const_iterator citer;
    typedef typename Container::iterator iter;
    citer end(vertices.end());
    for(iter v=vertices.begin(); v!=end; ++v) {
      const DirectedGraph::ver_ptr& vptr = vertex_ptr(*v);
      result->push_back(MemBlock((vptr)->address(),(vptr)->size(),false,SafePtr<MemBlock>(),SafePtr<MemBlock>()));
    }
    return result;
  }
};

void
DirectedGraph::print_def(const SafePtr<CodeContext>& context, std::ostream& os,
                        const SafePtr<ImplicitDimensions>& dims)
{
  std::ostringstream oss;
  const std::string null_str("");
  SafePtr<Entity > ctimeconst_zero(new CTimeEntity<int>(0));

  const bool accumulate_targets_directly = registry()->accumulate_targets() && iregistry()->accumulate_targets_directly();
  const bool accumulate_targets_indirectly = registry()->accumulate_targets() && !accumulate_targets_directly;

  //
  // To optimize accumulation and setting blocks to zero, need to merge maximally the targets (the accumulation area is one block)
  //
  SafePtr<MemBlockSet> targetblks;
  if (registry()->accumulate_targets()) {
    if (accumulate_targets_directly) {
      targetblks = to_memoryblks(targets_);
      merge(*targetblks);
    }
    else {
      targetblks = SafePtr<MemBlockSet>(new MemBlockSet);
      targetblks->push_back(MemBlock(0,iregistry()->size_of_target_accum(),false,SafePtr<MemBlock>(),SafePtr<MemBlock>()));
    }
  }

  //
  // If accumulating integrals, check inteval's zero_out_targets. If set to 1 -- zero out accumulated targets
  //
  if (registry()->accumulate_targets()) {

    const bool vecdim_is_static = dims->vecdim_is_static();

    os << "if (inteval->zero_out_targets) {" << std::endl;

    typedef MemBlockSet::const_iterator citer;
    typedef MemBlockSet::iterator iter;
    const citer end = targetblks->end();
    for(iter b=targetblks->begin(); b!=end; ++b) {

      size s = b->size();
      SafePtr<CTimeEntity<int> > bdim(new CTimeEntity<int>(s));

      SafePtr<Entity> bvecdim;
      if (!vecdim_is_static) {
	SafePtr< RTimeEntity<EntityTypes::Int> > vecdim = dynamic_pointer_cast<RTimeEntity<EntityTypes::Int>,Entity>(dims->vecdim());
	bvecdim = vecdim * bdim;
      }
      else {
	SafePtr< CTimeEntity<int> > vecdim = dynamic_pointer_cast<CTimeEntity<int>,Entity>(dims->vecdim());
	bvecdim = vecdim * bdim;
      }

#if 0
      std::string loopvar("i");
      ForLoop loop(context,loopvar,bvecdim,ctimeconst_zero);
      os << loop.open();
      {
	ostringstream oss;
	oss << registry()->stack_name() << "[" << loopvar << "]";
	const std::string zero("0");
	os << context->assign(oss.str(),zero);
      }
      os << loop.close();
#endif
      os << "_libint2_static_api_bzero_short_(" << registry()->stack_name() << "+"
	 << b->address() << "*" << dims->vecdim()->id() << "," << bvecdim->id() << ")" << endl;

    }

    os << "inteval->zero_out_targets = 0;" << std::endl << "}" << std::endl;
  }

  std::string varname("hsi");
  SafePtr<ForLoop> hsi_loop(new ForLoop(context,varname,dims->high(),SafePtr<Entity>(new CTimeEntity<int>(0))));
  os << hsi_loop->open();

  varname = "lsi";
  SafePtr<ForLoop> lsi_loop(new ForLoop(context,varname,dims->low(),SafePtr<Entity>(new CTimeEntity<int>(0))));
  os << lsi_loop->open();

  // the vector loop is created outside of the body of the function if
  // 1) blockwise vectorization is requested
  // and
  // 2) this is a purely int-unit code, i.e. there are no explicit RRs on sets in the body
  // Otherwise, I create a dummy vector loop with the vector loop index set to 0
  const unsigned int max_vector_length = context->cparams()->max_vector_length();
  const bool vectorize = (max_vector_length != 1);
  const bool vectorize_by_line = context->cparams()->vectorize_by_line();
  const bool create_outer_vector_loop = !vectorize_by_line && !contains_nontrivial_rr();
  varname = "vi";
  // outer vector loop
  SafePtr<ForLoop> outer_vloop;
  // vector loop for each code line
  SafePtr<ForLoop> line_vloop;
  if (create_outer_vector_loop) {
    SafePtr<ForLoop> tmp_vi_loop(new ForLoop(context,varname,dims->vecdim(),SafePtr<Entity>(new CTimeEntity<int>(0))));
    outer_vloop = tmp_vi_loop;
  }
  else {
    SafePtr<Entity> unit_dim(new CTimeEntity<int>(1));
    SafePtr<ForLoop> tmp_vi_loop(new ForLoop(context,varname,unit_dim,SafePtr<Entity>(new CTimeEntity<int>(0))));
    outer_vloop = tmp_vi_loop;
    SafePtr<ForLoop> tmp2_vi_loop(new ForLoop(context,varname,dims->vecdim(),SafePtr<Entity>(new CTimeEntity<int>(0))));
    line_vloop = tmp2_vi_loop;
    // note that both loops use same variable name -- standard C++ scoping rules allow it -- but the outer
    // loop will become a declaration of a constant variable
  }
  os << outer_vloop->open();

  //
  // generate code for vertices
  //
  unsigned int nflops_total = 0;
  SafePtr<DGVertex> current_vertex = first_to_compute_;
  do {

    // for every vertex that has a defined symbol, hence must be defined in code
    if (current_vertex->symbol_set()) {

      const bool address_set = current_vertex->address_set();

      // print algebraic expression
      {
        typedef AlgebraicOperator<DGVertex> oper_type;
        SafePtr<oper_type> oper_ptr = dynamic_pointer_cast<oper_type,DGVertex>(current_vertex);
        if (oper_ptr) {

          // Type declaration if this is an automatic variable (i.e. not on Libint's stack)
          if (!address_set) {
            os << context->declare(context->type_name<double>(),
                                   current_vertex->symbol());
#if CHECK_SAFETY
	    current_vertex->declared(true);
#endif
	  }


	  // If this is an Integral in a target IntegralSet AND
	  // can accumulate targets directly -- use '+=' instead of '='
	  const bool accumulate_not_assign = accumulate_targets_directly && IntegralInTargetIntegralSet()(current_vertex);

	  typedef DGVertex::ArcSetType::const_iterator aciter;
	  aciter a = oper_ptr->first_exit_arc();
	  const SafePtr<DGVertex>& left_arg = (*a)->dest();  ++a;
	  const SafePtr<DGVertex>& right_arg = (*a)->dest();

          if (context->comments_on()) {

            oss.str(null_str);
            oss << current_vertex->label() << (accumulate_not_assign ? " += " : " = ")
                << left_arg->label()
                << oper_ptr->label()
                << right_arg->label();
            os << context->comment(oss.str()) << endl;
          }

          // expression
#if DEBUG
          cout << "Generating code for " << current_vertex->description() << endl;
          cout << "              ptr = " << current_vertex << endl;
          cout << "         left_arg = " << left_arg->description() << endl;
          cout << "        right_arg = " << right_arg->description() << endl;
#endif

          // convert symbols to their vector form if needed
          std::string curr_symbol = current_vertex->symbol();
          std::string left_symbol = left_arg->symbol();
          std::string right_symbol = right_arg->symbol();
          if (vectorize) {
            curr_symbol = to_vector_symbol(current_vertex);
            left_symbol = to_vector_symbol(left_arg);
            right_symbol = to_vector_symbol(right_arg);
          }
#if CHECK_SAFETY
	  bool left_not_declared = left_arg->need_to_compute() && !left_arg->declared();
	  bool right_not_declared = right_arg->need_to_compute() && !right_arg->declared();
	  if (left_not_declared || right_not_declared) {
	    std::cout << "Current vertex:" << endl;  current_vertex->print(std::cout);
	    std::cout << "left arg      :" << endl;  left_arg->print(std::cout);
	    std::cout << "right arg     :" << endl;  right_arg->print(std::cout);
	    if (left_not_declared) throw ProgrammingError("DirectedGraph::print_def() -- left_arg not declared");
	    if (right_not_declared) throw ProgrammingError("DirectedGraph::print_def() -- right_arg not declared");
	  }
#endif

          if (vectorize_by_line)
            os << line_vloop->open();
	  // the statement that does the work
	  {
	    if (accumulate_not_assign) {
	      os << context->accumulate_binary_expr(curr_symbol,left_symbol,oper_ptr->label(),right_symbol);
	      nflops_total += (1 + nflops(left_symbol) + nflops(right_symbol)) + 1;
	    }
	    else {
	      os << context->assign_binary_expr(curr_symbol,left_symbol,oper_ptr->label(),right_symbol);
	      nflops_total += (1 + nflops(left_symbol) + nflops(right_symbol));
	    }
	  }
          if (vectorize_by_line)
            os << line_vloop->close();


          goto next;
        }
      }

      // print simple assignment statement
      if (current_vertex->num_exit_arcs() == 1) {
        typedef DGArcDirect arc_type;
        SafePtr<arc_type> arc_ptr = dynamic_pointer_cast<arc_type,DGArc>(*(current_vertex->first_exit_arc()));
        if (arc_ptr) {

#if CHECK_SAFETY
	    current_vertex->declared(true);

	    SafePtr<DGVertex> rhs_arg = arc_ptr->dest();
	    if (!rhs_arg->declared() && rhs_arg->need_to_compute()) {
	      std::cout << "Current vertex:" << endl;  current_vertex->print(std::cout);
	      std::cout << "rhs_arg      :" << endl;  rhs_arg->print(std::cout);
	      throw ProgrammingError("DirectedGraph::print_def() -- rhs_arg not declared");
	    }
#endif

	  // If this is an Integral in a target IntegralSet AND
	  // can accumulate targets directly -- use '+=' instead of '='
	  const bool accumulate_not_assign = accumulate_targets_directly && IntegralInTargetIntegralSet()(current_vertex);

          if (context->comments_on()) {
            oss.str(null_str);
            oss << current_vertex->label() << (accumulate_not_assign ? " += " : " = ")
                << arc_ptr->dest()->label();
            os << context->comment(oss.str()) << endl;
          }

          // convert symbols to their vector form if needed
          std::string curr_symbol = current_vertex->symbol();
          std::string rhs_symbol = arc_ptr->dest()->symbol();
          if (vectorize) {
            curr_symbol = to_vector_symbol(current_vertex);
            rhs_symbol = to_vector_symbol(arc_ptr->dest());
          }

          if (vectorize_by_line)
            os << line_vloop->open();
	  if (accumulate_not_assign) {
	    os << context->accumulate(curr_symbol,
				      rhs_symbol);
	    nflops_total += nflops(rhs_symbol) + 1;   // +1 due to +=
	  }
	  else {
	    os << context->assign(curr_symbol,
				  rhs_symbol);
	    nflops_total += nflops(rhs_symbol);
	  }
          if (vectorize_by_line)
            os << line_vloop->close();


          goto next;
        }
      }

      // print out a recurrence relation
      {
        typedef DGArcRR arc_type;
        SafePtr<arc_type> arc_ptr = dynamic_pointer_cast<arc_type,DGArc>(*(current_vertex->first_exit_arc()));
        if (arc_ptr) {

          SafePtr<RecurrenceRelation> rr = arc_ptr->rr();
          os << rr->spfunction_call(context,dims);

          goto next;
        }
      }

      {
        current_vertex->print(std::cout);
        throw std::runtime_error("DirectedGraph::print_def() -- cannot handle this vertex yet");
      }
    }

    next:
    current_vertex = current_vertex->postcalc();
  } while (current_vertex != 0);

  os << outer_vloop->close();
  os << lsi_loop->close();
  os << hsi_loop->close();

  //
  // Accumulate targets
  //
  if (accumulate_targets_indirectly) {
    os << context->comment("Accumulate target integral sets") << std::endl;
    const std::string& stack_name = registry()->stack_name();
    const bool vecdim_is_static = dims->vecdim_is_static();
#if 0
    unsigned int vecdim_rank;
    if (vecdim_is_static) {
      SafePtr<CTimeEntity<int> > cptr = dynamic_pointer_cast<CTimeEntity<int> ,Entity >(dims->vecdim());
      vecdim_rank = cptr->value();
    }
    const std::string times_vecdim("*" + dims->vecdim_label());
#endif

    // Loop over targets
    unsigned int curr_target = 0;
    const target_iter end = targets_.end();
    for(target_iter t=targets_.begin(); t!=targets_.end(); ++t, ++curr_target) {
      const ver_ptr& tptr = vertex_ptr(*t);

      size s = (tptr)->size();
      SafePtr<CTimeEntity<int> > bdim(new CTimeEntity<int>(s));

      SafePtr<Entity> bvecdim;
      if (!vecdim_is_static) {
	SafePtr< RTimeEntity<EntityTypes::Int> > vecdim = dynamic_pointer_cast<RTimeEntity<EntityTypes::Int>,Entity>(dims->vecdim());
	bvecdim = vecdim * bdim;
      }
      else {
	SafePtr< CTimeEntity<int> > vecdim = dynamic_pointer_cast<CTimeEntity<int>,Entity>(dims->vecdim());
	bvecdim = vecdim * bdim;
      }

      // For now write an explicit loop for each target. In the future should:
      // 1) check if all computed targets are adjacent (accumulated targets are adjacent by the virtue of the allocation mechanism)
      // 2) check the sizes and insert optimized calls, if possible

      // form a single loop over integrals and vector dimension
      // NOTE: single loop suffices because if outer/inner strides are not 1 this block of code should not be executed

#if 0
      SafePtr<Entity> loopmax;
      if (vecdim_is_static) {
	const int loopmax_value = s*vecdim_rank;
	loopmax = SafePtr<Entity>(new CTimeEntity<int>(loopmax_value));
      }
      else {
	ostringstream oss;  oss << s << times_vecdim;
	loopmax = SafePtr<Entity>(new RTimeEntity<EntityTypes::Int>(oss.str()));
      }

      std::string loopvar("i");
      ForLoop loop(context,loopvar,loopmax,ctimeconst_zero);
      os << loop.open();
      std::string acctarget;
      {
	ostringstream oss;
	oss << stack_name << "[" << target_accums_[curr_target] << times_vecdim << "+" << loopvar << "]";
	acctarget = oss.str();
      }
      std::string target;
      {
	ostringstream oss;
	oss << stack_name << "[" << (tptr)->address() << times_vecdim << "+" << loopvar << "]";
	target = oss.str();
      }
      os << context->accumulate(acctarget,target);
      os << loop.close();
#endif
      os << "_libint2_static_api_inc_short_("
	 << registry()->stack_name() << "+" << target_accums_[curr_target] << "*" << dims->vecdim()->id() << ","
	 << registry()->stack_name() << "+" << (tptr)->address() << "*" << dims->vecdim()->id() << ","
	 << bvecdim->id() << ","
	 << "1.0)" << endl;

      nflops_total += s;
    }
  }

  // Outside of loops stack symbols don't make sense, so we must define loop variables hsi, lsi, and vi to 0
  os << context->decldef(context->type_name<const int>(), "hsi", "0");
  os << context->decldef(context->type_name<const int>(), "lsi", "0");
  os << context->decldef(context->type_name<const int>(), "vi", "0");

  //
  // Now pass back all targets through the inteval object, if needed.
  //
  if (registry()->return_targets()) {
    unsigned int curr_target = 0;
    const target_iter end = targets_.end();
    for(target_iter t=targets_.begin(); t!=targets_.end(); ++t, ++curr_target) {
      const ver_ptr& tptr = vertex_ptr(*t);
      const std::string& symbol = (accumulate_targets_indirectly
				   //                                                                    is this correct?         ???
				   ? stack_symbol(context,target_accums_[curr_target],(tptr)->size(),dims->low_label(),dims->vecdim_label(),registry()->stack_name())
				   : (tptr)->symbol());
      os << "inteval->targets[" << curr_target << "] = "
	 << context->value_to_pointer(symbol) << context->end_of_stat() << endl;
    }
  }

  // Print out the number of flops
  oss.str(null_str);
  oss << "Number of flops = " << nflops_total;
  os << context->comment(oss.str()) << endl;

  if (context->cparams()->count_flops()) {
    oss.str(null_str);
    oss << nflops_total << " * " << dims->high_label() << " * "
        << dims->low_label() << " * "
        << dims->vecdim_label();
    os << context->assign_binary_expr("inteval->nflops","inteval->nflops","+",oss.str());
  }

}

void
DirectedGraph::update_func_names()
{
  // Loop over all vertices
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    // for every vertex with children
    if ((vptr)->num_exit_arcs() > 0) {
      // if it must be computed using a RR
      SafePtr<DGArc> arc = *((vptr)->first_exit_arc());
      SafePtr<DGArcRR> arcrr = dynamic_pointer_cast<DGArcRR,DGArc>(arc);
      if (arcrr != 0) {
        SafePtr<RecurrenceRelation> rr = arcrr->rr();
        // and the RR is complex (i.e. likely to result in a function call)
        if (!rr->is_simple())
          // add it to the RRStack
          func_names_[rr->label()] = true;
      }
    }
  }
}

bool
DirectedGraph::contains_nontrivial_rr() const
{
  SafePtr<DGVertex> current_vertex = first_to_compute_;
  do {
    const int nchildren = current_vertex->num_exit_arcs();
    if (nchildren > 0) {
      arc_ptr aptr = *(current_vertex->first_exit_arc());
      typedef RecurrenceRelation RR;
      SafePtr<DGArcRR> aptr_cast = dynamic_pointer_cast<DGArcRR,arc>(aptr);
      // if this is a RR
      if (aptr_cast != 0) {
        // and a non-trivial one
        if (!aptr_cast->rr()->is_simple())
          return true;
      }
    }
    current_vertex = current_vertex->postcalc();
  } while (current_vertex != 0);

  return false;
}

void
DirectedGraph::find_subtrees()
{
  // need to condense expressions?
  if (!registry()->condense_expr())
    return;

  // Find subtrees by starting from the targets and moving down ...
  typedef vertices::const_iterator citer;
  typedef vertices::iterator iter;
  for(iter v=stack_.begin(); v!=stack_.end(); ++v) {
    const ver_ptr& vptr = vertex_ptr(*v);
    if ((vptr)->is_a_target() && (vptr)->num_entry_arcs() == 0) {
      find_subtrees_from(vptr);
    }
  }
}

void
DirectedGraph::find_subtrees_from(const SafePtr<DGVertex>& v)
{
  // is not on a subtree already
  if (!v->subtree()) {

    bool useless_subtree = false;

    //
    // Subtrees are useless in the following cases:
    // 1) root is computed via RRs, not explicitly
    //
    {
      if (v->num_exit_arcs() > 0) {
        SafePtr<DGArc> arc = *(v->first_exit_arc());
        SafePtr<DGArcRR> arc_rr = dynamic_pointer_cast<DGArcRR,DGArc>(arc);
        if (arc_rr)
          useless_subtree = true;
      }
    }

    // create subtree
    if (!useless_subtree) {
      SafePtr<DRTree> stree = DRTree::CreateRootedAt(v);

      // Remove all trivial subtrees
      if (stree) {
#if DISABLE_SUBTREES
        if (stree->nvertices() >= 0) {
          stree->detach();
        }
#else
        if (stree->nvertices() < 3) {
          stree->detach();
        }
#endif
      }
    }

    // move on to children
    typedef DGVertex::ArcSetType::const_iterator aciter;
    const aciter abegin = v->first_exit_arc();
    const aciter aend = v->plast_exit_arc();
    for(aciter a=abegin; a!=aend; ++a) {
      find_subtrees_from((*a)->dest());
    }
  }
}

////

namespace libint2 {

  namespace {
#if USE_ASSOCCONTAINER_BASED_DIRECTEDGRAPH
    typedef DirectedGraph::targets::value_type value_type;
    struct __NotUnrolledIntegralSet : public std::unary_function<const value_type&,bool> {
      bool operator()(const value_type& v) {
	return NotUnrolledIntegralSet()(v);
      }
    };
#endif
  };

  bool
  nonunrolled_targets(const DirectedGraph::targets& targets) {
    typedef DirectedGraph::target_citer citer;
    citer end = targets.end();
#if USE_ASSOCCONTAINER_BASED_DIRECTEDGRAPH
    if (end != find_if(targets.begin(),end,__NotUnrolledIntegralSet()))
#else
    if (end != find_if(targets.begin(),end,NotUnrolledIntegralSet()))
#endif
      return true;
    else
      return false;
  }

  void
  extract_symbols(const SafePtr<DirectedGraph>& dg)
  {
    LibraryTaskManager& taskmgr = LibraryTaskManager::Instance();
    // symbol extractor
    {
      SafePtr<ExtractExternSymbols> extractor(new ExtractExternSymbols);
      dg->foreach(*extractor);
      const ExtractExternSymbols::Symbols& symbols = extractor->symbols();
      // pass on to the symbol maintainer of the current task
      taskmgr.current().symbols()->add(symbols);
#if DEBUG
      // print out the symbols
      std::cout << "Recovered symbols from DirectedGraph for " << dg << std::endl;
      typedef ExtractExternSymbols::Symbols::const_iterator citer;
      citer end = symbols.end();
      for(citer t=symbols.begin(); t!=end; ++t)
	std::cout << *t << std::endl;
#endif
    }
    // RR extractor
    {
      SafePtr<ExtractRR> extractor(new ExtractRR);
      dg->foreach(*extractor);
      const ExtractRR::RRList& rrlist = extractor->rrlist();
      // pass on to the symbol maintainer of the current task
      taskmgr.current().symbols()->add(rrlist);
    }
  }

};