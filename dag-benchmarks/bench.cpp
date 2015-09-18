/*!
 * \file bench.cpp
 * \brief Benchmarking script for DAG machine
 * \date 2015
 * \copyright COPYRIGHT (c) 2015 Umut Acar, Arthur Chargueraud, and
 * Michael Rainey. All rights reserved.
 * \license This project is released under the GNU Public License.
 *
 */

#include <random>
#include <unordered_map>
#include <set>
#include <deque>
#include <cmath>
#include <memory>
#include <chrono>
#include <thread>

#include "pasl.hpp"
#include "tagged.hpp"
#include "snzi.hpp"

/***********************************************************************/

/*---------------------------------------------------------------------*/
/* Tagged-pointer routines */

template <class T>
T* tagged_pointer_of(T* n) {
  return pasl::data::tagged::extract_value<T*>(n);
}

template <class T>
int tagged_tag_of(T* n) {
  return (int)pasl::data::tagged::extract_tag<long>(n);
}

template <class T>
T* tagged_tag_with(T* n, int t) {
  return pasl::data::tagged::create<T*, T*>(n, (long)t);
}

/*---------------------------------------------------------------------*/


/*---------------------------------------------------------------------*/
/* Random-number generator */

pasl::data::perworker::array<std::mt19937> generator;

// returns a random integer in the range [lo, hi)
int random_int(int lo, int hi) {
  std::uniform_int_distribution<int> distribution(lo, hi-1);
  return distribution(generator.mine());
}

/*---------------------------------------------------------------------*/


/*---------------------------------------------------------------------*/
/* Global parameters */

int communication_delay = 100;

using port_passing_mode = enum {
  port_passing_default,
  port_passing_intersection,
  port_passing_difference
};

/*---------------------------------------------------------------------*/


/*---------------------------------------------------------------------*/
/* The top-down algorithm */

namespace direct {

class node;
class incounter;
class outset;

void add_node(node*);
void add_edge(node*, node*);
void add_edge(node*, outset*, node*, incounter*);
void prepare_node(node*);
void prepare_node(node*, incounter*);
void prepare_node(node*, outset*);
void prepare_node(node*, incounter*, outset*);
void join_with(node*, incounter*);
void continue_with(node*);
void decrement_incounter(node*);
incounter* incounter_ready();
incounter* incounter_unary();
incounter* incounter_fetch_add();
incounter* incounter_new(node*);
outset* outset_unary();
outset* outset_noop();
outset* outset_new();
template <class Body>
node* new_parallel_for(long, long, node*, const Body&);
  
class incounter : public pasl::sched::instrategy::common {
public:
  
  using status_type = enum {
    activated,
    not_activated
  };
  
  virtual bool is_activated() const = 0;
  
  virtual void increment(node* source) = 0;
  
  virtual status_type decrement(node* source) = 0;
  
  void check(pasl::sched::thread_p t) {
    if (is_activated()) {
      start(t);
    }
  }
  
  void delta(node* source, pasl::sched::thread_p target, int64_t d) {
    if (d == -1L) {
      if (decrement(source) == activated) {
        start(target);
      }
    } else if (d == +1L) {
      increment(source);
    } else {
      assert(false);
    }
  }
  
  void delta(pasl::sched::thread_p target, int64_t d) {
    delta(nullptr, target, d);
  }
  
};

class outset : public pasl::sched::outstrategy::common {
public:
  
  using insert_status_type = enum {
    insert_success,
    insert_fail
  };
  
  virtual insert_status_type insert(node*) = 0;
  
  virtual void finish() = 0;
  
  virtual void destroy() {
    delete this;
  }
  
  void add(pasl::sched::thread_p t) {
    insert((node*)t);
  }
  
  void finished() {
    finish();
  }
  
  bool should_deallocate_automatically = true;
  
  virtual void enable_future() {
    should_deallocate_automatically = false;
  }
  
};

using edge_algorithm_type = enum {
  edge_algorithm_simple,
  edge_algorithm_distributed,
  edge_algorithm_tree
};
  
edge_algorithm_type edge_algorithm;
  
namespace simple {
  
class simple_outset : public outset {
public:
  
  using concurrent_list_type = struct concurrent_list_struct {
    node* n;
    struct concurrent_list_struct* next;
  };
  
  std::atomic<concurrent_list_type*> head;
  
  const int finished_code = 1;
  
  simple_outset() {
    head.store(nullptr);
  }
  
  insert_status_type insert(node* n) {
    insert_status_type result = insert_success;
    concurrent_list_type* cell = new concurrent_list_type;
    cell->n = n;
    while (true) {
      concurrent_list_type* orig = head.load();
      if (tagged_tag_of(orig) == finished_code) {
        result = insert_fail;
        delete cell;
        break;
      } else {
        cell->next = orig;
        if (head.compare_exchange_strong(orig, cell)) {
          break;
        }
      }
    }
    return result;
  }
  
  void finish() {
    concurrent_list_type* todo = nullptr;
    while (true) {
      concurrent_list_type* orig = head.load();
      concurrent_list_type* v = (concurrent_list_type*)nullptr;
      concurrent_list_type* next = tagged_tag_with(v, finished_code);
      if (head.compare_exchange_strong(orig, next)) {
        todo = orig;
        break;
      }
    }
    while (todo != nullptr) {
      node* n = todo->n;
      concurrent_list_type* next = todo->next;
      delete todo;
      decrement_incounter(n);
      todo = next;
    }
    if (should_deallocate_automatically) {
      delete this;
    }
  }
  
};
  
} // end namespace
  
namespace distributed {
  
int default_branching_factor = 2;
int default_nb_levels = 3;
  
class distributed_incounter : public incounter {
public:
      
  pasl::data::snzi::tree nzi;
  
  distributed_incounter(node* n)
  : nzi(default_branching_factor, default_nb_levels) {
    nzi.set_root_annotation(n);
  }
  
  bool is_activated() const {
    return (! nzi.is_nonzero());
  }
  
  void increment(node* source) {
    nzi.random_leaf_of(source)->arrive();
  }
  
  status_type decrement(node* source) {
    bool b = nzi.random_leaf_of(source)->depart();
    return b ? incounter::activated : incounter::not_activated;
  }
  
};
  
void unary_finished(pasl::sched::thread_p t) {
  pasl::data::snzi::node* leaf = (pasl::data::snzi::node*)t;
  if (leaf->depart()) {
    node* n = pasl::data::snzi::node::get_root_annotation<node*>(leaf);
    pasl::sched::instrategy::schedule((pasl::sched::thread_p)n);
  }
}
  
} // end namespace
  
namespace dyntree {
  
int branching_factor = 2;
  
class dyntree_incounter;
class dyntree_outset;
class incounter_node;
class outset_node;

void deallocate_incounter_tree(incounter_node*);
void notify_outset_nodes(dyntree_outset*);
void deallocate_outset_tree(outset_node*);
  
class incounter_node {
public:
  
  static constexpr int minus_tag = 1;
  
  std::atomic<incounter_node*>* children;
  
  void init(incounter_node* v) {
    children = new std::atomic<incounter_node*>[branching_factor];
    for (int i = 0; i < branching_factor; i++) {
      children[i].store(v);
    }
  }
  
  incounter_node() {
    init(nullptr);
  }
  
  incounter_node(incounter_node* i) {
    init(i);
  }
  
  ~incounter_node() {
    delete [] children;
  }
  
  bool is_leaf() const {
    for (int i = 0; i < branching_factor; i++) {
      incounter_node* child = tagged_pointer_of(children[i].load());
      if (child != nullptr) {
        return false;
      }
    }
    return true;
  }
  
};

class dyntree_incounter : public incounter {
public:
  
  incounter_node* in;
  incounter_node* out;
  
  incounter_node* minus() const {
    return tagged_tag_with((incounter_node*)nullptr, incounter_node::minus_tag);
  }
  
  dyntree_incounter() {
    in = nullptr;
    out = new incounter_node(minus());
    out = tagged_tag_with(out, incounter_node::minus_tag);
  }
  
  ~dyntree_incounter() {
    assert(is_activated());
    deallocate_incounter_tree(tagged_pointer_of(out));
    out = nullptr;
  }
  
  bool is_activated() const {
    return in == nullptr;
  }
  
  void increment(node*) {
    incounter_node* leaf = new incounter_node;
    while (true) {
      if (in == nullptr) {
        in = leaf;
        return;
      }
      assert(in != nullptr);
      incounter_node* current = in;
      while (true) {
        int i = random_int(0, branching_factor);
        std::atomic<incounter_node*>& branch = current->children[i];
        incounter_node* next = branch.load();
        if (tagged_tag_of(next) == incounter_node::minus_tag) {
          break;
        }
        if (next == nullptr) {
          incounter_node* orig = nullptr;
          if (branch.compare_exchange_strong(orig, leaf)) {
            return;
          } else {
            break;
          }
        }
        current = next;
      }
    }
  }
  
  status_type decrement(node*) {
    while (true) {
      incounter_node* current = in;
      assert(current != nullptr);
      if (current->is_leaf()) {
        if (try_to_detatch(current)) {
          in = nullptr;
          add_to_out(current);
          return activated;
        }
      }
      while (true) {
        int i = random_int(0, branching_factor);
        std::atomic<incounter_node*>& branch = current->children[i];
        incounter_node* next = branch.load();
        if (   (next == nullptr)
            || (tagged_tag_of(next) == incounter_node::minus_tag) ) {
          break;
        }
        if (next->is_leaf()) {
          if (try_to_detatch(next)) {
            branch.store(nullptr);
            add_to_out(next);
            return not_activated;
          }
          break;
        }
        current = next;
      }
    }
    return not_activated;
  }
  
  bool try_to_detatch(incounter_node* n) {
    for (int i = 0; i < branching_factor; i++) {
      incounter_node* orig = nullptr;
      if (! (n->children[i].compare_exchange_strong(orig, minus()))) {
        for (int j = i - 1; j >= 0; j--) {
          n->children[j].store(nullptr);
        }
        return false;
      }
    }
    return true;
  }
  
  void add_to_out(incounter_node* n) {
    n = tagged_tag_with(n, incounter_node::minus_tag);
    while (true) {
      incounter_node* current = tagged_pointer_of(out);
      while (true) {
        int i = random_int(0, branching_factor);
        std::atomic<incounter_node*>& branch = current->children[i];
        incounter_node* next = branch.load();
        if (tagged_pointer_of(next) == nullptr) {
          incounter_node* orig = next;
          if (branch.compare_exchange_strong(orig, n)) {
            return;
          }
          break;
        }
        current = tagged_pointer_of(next);
      }
    }
  }
  
};
  
class outset_node {
public:
  
  enum {
    empty=1,
    leaf=2,
    interior=3,
    finished_empty=4,
    finished_leaf=5,
    finished_interior=6
  };
  
  using tagged_pointer_type = union {
    outset_node* interior;
    node* leaf;
  };
  
  std::atomic<tagged_pointer_type>* children;
  
  void init() {
    children = new std::atomic<tagged_pointer_type>[branching_factor];
    for (int i = 0; i < branching_factor; i++) {
      tagged_pointer_type p;
      p.interior = tagged_tag_with((outset_node*)nullptr, empty);
      children[i].store(p);
    }
  }
  
  outset_node() {
    init();
  }
  
  outset_node(tagged_pointer_type child1, tagged_pointer_type child2) {
    init();
    children[0].store(child1);
    children[1].store(child2);
  }
  
  ~outset_node() {
    delete [] children;
  }
  
  static tagged_pointer_type make_finished(tagged_pointer_type p) {
    tagged_pointer_type result;
    int tag = tagged_tag_of(p.interior);
    if (tag == empty) {
      outset_node* n = tagged_pointer_of(p.interior);
      result.interior = tagged_tag_with(n, finished_empty);
    } else if (tag == leaf) {
      node* n = tagged_pointer_of(p.leaf);
      result.leaf = tagged_tag_with(n, finished_leaf);
    } else if (tag == interior) {
      outset_node* n = tagged_pointer_of(p.interior);
      result.interior = tagged_tag_with(n, finished_interior);
    } else {
      assert(false);
    }
    return result;
  }
  
};
  
class dyntree_outset : public outset {
public:
 
  outset_node* root;
  
  dyntree_outset() {
    root = new outset_node;
  }
  
  ~dyntree_outset() {
    deallocate_outset_tree(root);
  }
  
  insert_status_type insert(outset_node::tagged_pointer_type val) {
    outset_node* current = root;
    outset_node* next = nullptr;
    while (true) {
      outset_node::tagged_pointer_type n;
      while (true) {
        int i = random_int(0, branching_factor);
        n = current->children[i].load();
        int tag = tagged_tag_of(n.interior);
        if (   (tag == outset_node::finished_empty)
            || (tag == outset_node::finished_leaf)
            || (tag == outset_node::finished_interior) ) {
          return insert_fail;
        }
        if (tag == outset_node::empty) {
          outset_node::tagged_pointer_type orig = n;
          if (current->children[i].compare_exchange_strong(orig, val)) {
            return insert_success;
          }
          n = current->children[i].load();
          tag = tagged_tag_of(n.interior);
        }
        if (tag == outset_node::leaf) {
          outset_node::tagged_pointer_type orig = n;
          outset_node* tmp = new outset_node(val, n);
          outset_node::tagged_pointer_type next;
          next.interior = tagged_tag_with(tmp, outset_node::interior);
          if (current->children[i].compare_exchange_strong(orig, next)) {
            return insert_success;
          }
          delete tmp;
          n = current->children[i].load();
          tag = tagged_tag_of(n.interior);
        }
        if (tag == outset_node::interior) {
          next = tagged_pointer_of(n.interior);
          break;
        }
      }
      current = next;
    }
    assert(false); // control should never reach here
    return insert_fail;
  }
  
  insert_status_type insert(node* leaf) {
    outset_node::tagged_pointer_type val;
    val.leaf = tagged_tag_with(leaf, outset_node::leaf);
    return insert(val);
  }

  void add(pasl::sched::thread_p t) {
    assert(false);
  }
  
  void finish() {
    notify_outset_nodes(this);
  }

};
  
} // end namespace
  
class node : public pasl::sched::thread {
public:
  
  using incounter_type = incounter;
  using outset_type = outset;
  
  const int uninitialized_block_id = -1;
  const int entry_block_id = 0;
  
  int current_block_id;
  
private:
  
  int continuation_block_id;
  
public:
  
  node()
  : current_block_id(uninitialized_block_id),
    continuation_block_id(entry_block_id) { }
  
  virtual void body() = 0;
  
  void run() {
    current_block_id = continuation_block_id;
    continuation_block_id = uninitialized_block_id;
    assert(current_block_id != uninitialized_block_id);
    body();
  }
  
  void prepare_for_transfer(int target) {
    pasl::sched::threaddag::reuse_calling_thread();
    continuation_block_id = target;
  }
  
  void jump_to(int continuation_block_id) {
    prepare_for_transfer(continuation_block_id);
    continue_with(this);
  }
  
  void async(node* producer, node* consumer, int continuation_block_id) {
    prepare_node(producer, incounter_ready(), outset_unary());
    add_edge(producer, consumer);
    jump_to(continuation_block_id);
    add_node(producer);
  }
  
  void finish(node* producer, int continuation_block_id) {
    node* consumer = this;
    prepare_node(producer, incounter_ready(), outset_unary());
    prepare_for_transfer(continuation_block_id);
    join_with(consumer, incounter_new(this));
    add_edge(producer, consumer);
    add_node(producer);
  }
  
  static outset* allocate_future() {
    outset* out = outset_new();
    out->enable_future();
    return out;
  }
  
  void listen_on(outset*) {
    // nothing to do
  }
  
  void future(node* producer, outset* producer_out, int continuation_block_id) {
    node* consumer = this;
    prepare_node(producer, incounter_ready(), producer_out);
    consumer->jump_to(continuation_block_id);
    add_node(producer);
  }
  
  outset* future(node* producer, int continuation_block_id) {
    outset* producer_out = allocate_future();
    future(producer, producer_out, continuation_block_id);
    return producer_out;
  }
  
  void force(outset* producer_out, int continuation_block_id) {
    node* consumer = this;
    prepare_for_transfer(continuation_block_id);
    incounter* consumer_in = incounter_unary();
    join_with(consumer, consumer_in);
    node* producer = nullptr; // dummy value
    add_edge(producer, producer_out, consumer, consumer_in);
  }
  
  void deallocate_future(outset* future) const {
    assert(! future->should_deallocate_automatically);
    future->destroy();
  }
  
  template <class Body>
  void parallel_for(long lo, long hi, const Body& body, int continuation_block_id) {
    node* consumer = this;
    node* producer = new_parallel_for(lo, hi, consumer, body);
    prepare_node(producer, incounter_ready(), outset_unary());
    prepare_for_transfer(continuation_block_id);
    join_with(consumer, incounter_new(this));
    add_edge(producer, consumer);
    add_node(producer);
  }
  
  void split_with(node*) {
    // nothing to do
  }
  
  void call(node* target, int continuation_block_id) {
    finish(target, continuation_block_id);
  }
  
  void detach(int continuation_block_id) {
    prepare_for_transfer(continuation_block_id);
    join_with(this, incounter_ready());
  }
  
  void set_inport_mode(port_passing_mode mode) {
    // nothing to do
  }
  
  void set_outport_mode(port_passing_mode mode) {
    // nothing to do
  }
  
  THREAD_COST_UNKNOWN
  
};

incounter* incounter_ready() {
  return (incounter*)pasl::sched::instrategy::ready_new();
}

incounter* incounter_unary() {
  return (incounter*)pasl::sched::instrategy::unary_new();
}
  
incounter* incounter_fetch_add() {
  return (incounter*)pasl::sched::instrategy::fetch_add_new();
}
  
incounter* incounter_new(node* n) {
  if (edge_algorithm == edge_algorithm_simple) {
    return incounter_fetch_add();
  } else if (edge_algorithm == edge_algorithm_distributed) {
    return new distributed::distributed_incounter(n);
  } else if (edge_algorithm == edge_algorithm_tree) {
    return new dyntree::dyntree_incounter;
  } else {
    assert(false);
    return nullptr;
  }
}
  
const bool enable_distributed = true;

outset* outset_unary() {
  if (enable_distributed && edge_algorithm == edge_algorithm_distributed) {
    return (outset*)pasl::sched::outstrategy::direct_distributed_unary_new(nullptr);
  } else {
    return (outset*)pasl::sched::outstrategy::unary_new();
  }
}
  
outset* outset_noop() {
  return (outset*)pasl::sched::outstrategy::noop_new();
}

outset* outset_new() {
  if (edge_algorithm == edge_algorithm_simple) {
    return new simple::simple_outset;
  } else if (edge_algorithm == edge_algorithm_distributed) {
    return new dyntree::dyntree_outset;
  } else if (edge_algorithm == edge_algorithm_tree) {
    return new dyntree::dyntree_outset;
  } else {
    assert(false);
    return nullptr;
  }
}
  
void increment_incounter(node* source, node* target, incounter* target_in) {
  long tag = pasl::sched::instrategy::extract_tag(target_in);
  assert(tag != pasl::sched::instrategy::READY_TAG);
  if (tag == pasl::sched::instrategy::UNARY_TAG) {
    // nothing to do
  } else if (tag == pasl::sched::instrategy::FETCH_ADD_TAG) {
    pasl::data::tagged::atomic_fetch_and_add<pasl::sched::instrategy_p>(&(target->in), 1l);
  } else {
    assert(tag == 0);
    source = enable_distributed ? source : nullptr;
    target_in->delta(source, target, +1L);
  }
}
  
void increment_incounter(node* source, node* target) {
  increment_incounter(source, target, (incounter*)target->in);
}
  
void decrement_incounter(node* source, node* target, incounter* target_in) {
  long tag = pasl::sched::instrategy::extract_tag(target_in);
  assert(tag != pasl::sched::instrategy::READY_TAG);
  if (tag == pasl::sched::instrategy::UNARY_TAG) {
    pasl::sched::instrategy::schedule(target);
  } else if (tag == pasl::sched::instrategy::FETCH_ADD_TAG) {
    int64_t old = pasl::data::tagged::atomic_fetch_and_add<pasl::sched::instrategy_p>(&(target->in), -1l);
    if (old == 1) {
      pasl::sched::instrategy::schedule(target);
    }
  } else {
    assert(tag == 0);
    source = enable_distributed ? source : nullptr;
    target_in->delta(source, target, -1L);
  }
}

void decrement_incounter(node* source, node* target) {
  decrement_incounter(source, target, (incounter*)target->in);
}
  
void decrement_incounter(node* target) {
  decrement_incounter((node*)nullptr, target);
}
  
void add_node(node* n) {
  pasl::sched::threaddag::add_thread(n);
}
  
outset::insert_status_type outset_insert(node* source, outset* source_out, node* target) {
  long tag = pasl::sched::outstrategy::extract_tag(source_out);
  assert(tag != pasl::sched::outstrategy::NOOP_TAG);
  if (tag == pasl::sched::outstrategy::UNARY_TAG) {
    source->out = pasl::data::tagged::create<pasl::sched::thread_p, pasl::sched::outstrategy_p>(target, tag);
    return outset::insert_success;
  } else if (tag == pasl::sched::outstrategy::DIRECT_DISTRIBUTED_UNARY_TAG) {
    auto target_in = (distributed::distributed_incounter*)target->in;
    long tg = pasl::sched::instrategy::extract_tag(target_in);
    if ((tg == 0) && (edge_algorithm == edge_algorithm_distributed)) {
      auto leaf = target_in->nzi.random_leaf_of(source);
      auto t = (pasl::sched::thread_p)leaf;
      source->out = pasl::sched::outstrategy::direct_distributed_unary_new(t);
    } else {
      tag = pasl::sched::outstrategy::UNARY_TAG;
      source->out = pasl::data::tagged::create<pasl::sched::thread_p, pasl::sched::outstrategy_p>(target, tag);
    }
    return outset::insert_success;
  } else {
    assert(tag == 0);
    return source_out->insert(target);
  }
}

void add_edge(node* source, outset* source_out, node* target, incounter* target_in) {
  increment_incounter(source, target, target_in);
  if (outset_insert(source, source_out, target) == outset::insert_fail) {
    decrement_incounter(source, target, target_in);
  }
}
  
void add_edge(node* source, node* target) {
  add_edge(source, (outset*)source->out, target, (incounter*)target->in);
}
  
void prepare_node(node* n) {
  prepare_node(n, incounter_new(n), outset_new());
}

void prepare_node(node* n, incounter* in) {
  prepare_node(n, in, outset_new());
}

void prepare_node(node* n, outset* out) {
  prepare_node(n, incounter_new(n), out);
}
  
void prepare_node(node* n, incounter* in, outset* out) {
  n->set_instrategy(in);
  n->set_outstrategy(out);
}

outset* capture_outset() {
  auto sched = pasl::sched::threaddag::my_sched();
  outset* out = (outset*)sched->get_outstrategy();
  assert(out != nullptr);
  sched->set_outstrategy(outset_noop());
  return out;
}

void join_with(node* n, incounter* in) {
  prepare_node(n, in, capture_outset());
}

void continue_with(node* n) {
  join_with(n, incounter_ready());
  add_node(n);
}
  
template <class Body>
class lazy_parallel_for_rec : public node {
public:
  
  long lo;
  long hi;
  node* join;
  Body _body;
  
  lazy_parallel_for_rec(long lo, long hi, node* join, const Body& _body)
  : lo(lo), hi(hi), join(join), _body(_body) { }
  
  enum {
    process_block,
    repeat_block,
    exit
  };
  
  void body() {
    switch (node::current_block_id) {
      case process_block: {
        long n = std::min(hi, lo + (long)communication_delay);
        long i;
        for (i = lo; i < n; i++) {
          _body(i);
        }
        lo = i;
        node::jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (lo < hi) {
          node::jump_to(process_block);
        }
        break;
      }
    }
  }
  
  size_t size() const {
    return hi - lo;
  }
  
  pasl::sched::thread_p split() {
    long mid = (hi + lo) / 2;
    lazy_parallel_for_rec n = new lazy_parallel_for_rec(mid, hi, join, _body);
    hi = mid;
    add_edge(n, join);
    return n;
  }
  
};
  
template <class Body>
node* new_parallel_for(long lo, long hi, node* join, const Body& body) {
  return new lazy_parallel_for_rec<Body>(lo, hi, join, body);
}
  
namespace dyntree {
  
void deallocate_incounter_tree_partial(std::deque<incounter_node*>& todo) {
  int k = 0;
  while ( (k < communication_delay) && (! todo.empty()) ) {
    incounter_node* current = todo.back();
    todo.pop_back();
    for (int i = 0; i < branching_factor; i++) {
      incounter_node* child = tagged_pointer_of(current->children[i].load());
      if (child == nullptr) {
        continue;
      }
      todo.push_back(child);
    }
    delete current;
    k++;
  }
}
  
class deallocate_incounter_tree_par : public node {
public:
  
  using self_type = deallocate_incounter_tree_par;
  
  enum {
    process_block=0,
    repeat_block
  };
  
  std::deque<incounter_node*> todo;
  
  void body() {
    switch (current_block_id) {
      case process_block: {
        deallocate_incounter_tree_partial(todo);
        jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (! todo.empty()) {
          jump_to(process_block);
        }
        break;
      }
      default:
        break;
    }
  }
  
  size_t size() const {
    return todo.size();
  }
  
  pasl::sched::thread_p split() {
    assert(size() >= 2);
    auto n = todo.front();
    todo.pop_front();
    auto t = new self_type();
    t->todo.push_back(n);
    return t;
  }
  
};
  
void deallocate_incounter_tree(incounter_node* root) {
  deallocate_incounter_tree_par d;
  d.todo.push_back(root);
  deallocate_incounter_tree_partial(d.todo);
  if (! d.todo.empty()) {
    node* n = new deallocate_incounter_tree_par(d);
    prepare_node(n);
    add_node(n);
  }
}
  
void notify_outset_tree_nodes_partial(std::deque<outset_node*>& todo) {
  int k = 0;
  while ( (k < communication_delay) && (! todo.empty()) ) {
    outset_node* current = todo.back();
    todo.pop_back();
    for (int i = 0; i < branching_factor; i++) {
      outset_node::tagged_pointer_type n;
      while (true) {
        n = current->children[i].load();
        outset_node::tagged_pointer_type orig = n;
        outset_node::tagged_pointer_type next = outset_node::make_finished(n);
        if (current->children[i].compare_exchange_strong(orig, next)) {
          break;
        }
      }
      int tag = tagged_tag_of(n.leaf);
      if (tag == outset_node::leaf) {
        decrement_incounter(tagged_pointer_of(n.leaf));
      } else if (tag == outset_node::interior) {
        todo.push_back(tagged_pointer_of(n.interior));
      }
    }
    k++;
  }
}
  
class notify_outset_tree_nodes_par_rec : public node {
public:
  
  using self_type = notify_outset_tree_nodes_par_rec;
  
  enum {
    process_block=0,
    repeat_block,
    exit_block
  };

  node* join;
  std::deque<outset_node*> todo;
  
  notify_outset_tree_nodes_par_rec(node* join, outset_node* n)
  : join(join) {
    todo.push_back(n);
  }
  
  notify_outset_tree_nodes_par_rec(node* join, std::deque<outset_node*>& _todo)
  : join(join) {
    _todo.swap(todo);
  }
  
  void body() {
    switch (current_block_id) {
      case process_block: {
        notify_outset_tree_nodes_partial(todo);
        jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (! todo.empty()) {
          jump_to(process_block);
        }
        break;
      }
      default:
        break;
    }
  }
  
  size_t size() const {
    return todo.size();
  }
  
  pasl::sched::thread_p split() {
    assert(size() >= 2);
    auto n = todo.front();
    todo.pop_front();
    auto t = new self_type(join, n);
    add_edge(t, join);
    return t;
  }
  
};
  
class notify_outset_tree_nodes_par : public node {
public:
  
  using self_type = notify_outset_tree_nodes_par;
  
  enum {
    entry_block=0,
    exit_block
  };
  
  dyntree_outset* out;
  std::deque<outset_node*> todo;
  
  notify_outset_tree_nodes_par(dyntree_outset* out, std::deque<outset_node*>& _todo)
  : out(out) {
    todo.swap(_todo);
  }
  
  void body() {
    switch (current_block_id) {
      case entry_block: {
        finish(new notify_outset_tree_nodes_par_rec(this, todo),
               exit_block);
        break;
      }
      case exit_block: {
        if (out->should_deallocate_automatically) {
          delete out;
        }
        break;
      }
      default:
        break;
    }
  }
  
};
  
void notify_outset_nodes(dyntree_outset* out) {
  std::deque<outset_node*> todo;
  todo.push_back(out->root);
  notify_outset_tree_nodes_partial(todo);
  if (! todo.empty()) {
    auto n = new notify_outset_tree_nodes_par(out, todo);
    prepare_node(n);
    add_node(n);
  } else {
    if (out->should_deallocate_automatically) {
      delete out;
    }
  }
}
  
void deallocate_outset_tree_partial(std::deque<outset_node*>& todo) {
  int k = 0;
  while ( (k < communication_delay) && (! todo.empty()) ) {
    outset_node* n = todo.back();
    todo.pop_back();
    for (int i = 0; i < branching_factor; i++) {
      outset_node::tagged_pointer_type c = n->children[i].load();
      int tag = tagged_tag_of(c.interior);
      if (   (tag == outset_node::finished_empty)
          || (tag == outset_node::finished_leaf) ) {
        // nothing to do
      } else if (tag == outset_node::finished_interior) {
        todo.push_back(tagged_pointer_of(c.interior));
      } else {
        // should not occur, given that finished() has been called
        assert(false);
      }
    }
    delete n;
    k++;
  }
}
  
class deallocate_outset_tree_par : public node {
public:
  
  using self_type = deallocate_outset_tree_par;
  
  enum {
    process_block=0,
    repeat_block
  };
  
  std::deque<outset_node*> todo;
  
  void body() {
    switch (current_block_id) {
      case process_block: {
        deallocate_outset_tree_partial(todo);
        jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (! todo.empty()) {
          jump_to(process_block);
        }
        break;
      }
      default:
        break;
    }
  }
  
  size_t size() const {
    return todo.size();
  }
  
  pasl::sched::thread_p split() {
    assert(size() >= 2);
    auto n = todo.front();
    todo.pop_front();
    auto t = new self_type();
    t->todo.push_back(n);
    return t;
  }
  
};
  
void deallocate_outset_tree(outset_node* root) {
  deallocate_outset_tree_par d;
  d.todo.push_back(root);
  deallocate_outset_tree_partial(d.todo);
  if (! d.todo.empty()) {
    node* n = new deallocate_outset_tree_par(d);
    prepare_node(n);
    add_node(n);
  }
}
  
} // end namespace
  
} // end namespace

/*---------------------------------------------------------------------*/


/*---------------------------------------------------------------------*/
/* The bottom-up algorithm */

namespace portpassing {
  
class node;
class incounter;
class outset;
class incounter_node;
class outset_node;
  
using inport_map_type = std::unordered_map<incounter*, incounter_node*>;
using outport_map_type = std::unordered_map<outset*, outset_node*>;
  
void prepare_node(node*);
void prepare_node(node*, incounter*);
void prepare_node(node*, outset*);
void prepare_node(node*, incounter*, outset*);
incounter* incounter_ready();
incounter* incounter_unary();
incounter* incounter_fetch_add();
incounter* incounter_new(node*);
outset* outset_unary(node*);
outset* outset_noop();
outset* outset_new(node*);
void add_node(node* n);
void propagate_ports_for(node*, node*);
outset_node* find_outport(node*, outset*);
outset* capture_outset();
void join_with(node*, incounter*);
void continue_with(node*);
incounter_node* increment_incounter(node*);
std::pair<incounter_node*, incounter_node*> increment_incounter(node*, incounter_node*);
std::pair<incounter_node*, incounter_node*> increment_incounter(node*, node*);
void decrement_incounter(node*, incounter_node*);
void decrement_incounter(node*, incounter*, incounter_node*);
void decrement_inports(node*);
void insert_inport(node*, incounter*, incounter_node*);
void insert_inport(node*, node*, incounter_node*);
void insert_outport(node*, outset*, outset_node*);
void insert_outport(node*, node*, outset_node*);
void deallocate_future(node*, outset*);
void notify_outset_tree_nodes(outset*);
void deallocate_outset_tree(outset_node*);
template <class Body>
node* new_parallel_for(long, long, node*, const Body&);
  
class incounter_node {
public:
  
  incounter_node* parent;
  std::atomic<int> nb_removed_children;
  
  incounter_node() {
    parent = nullptr;
    nb_removed_children.store(0);
  }
  
};

class outset_node {
public:
  
  node* target;
  incounter_node* port;
  std::atomic<outset_node*> children[2];
  
  outset_node() {
    target = nullptr;
    port = nullptr;
    for (int i = 0; i < 2; i++) {
      children[i].store(nullptr);
    }
  }
  
};

class incounter : public pasl::sched::instrategy::common {
public:
  
  using status_type = enum {
    activated,
    not_activated
  };
  
  node* n;
  
  incounter(node* n)
  : n(n) {
    assert(n != nullptr);
  }
  
  bool is_activated(incounter_node* port) const {
    return port->parent == nullptr;
  }
  
  std::pair<incounter_node*, incounter_node*> increment(incounter_node* port) const {
    incounter_node* branch1;
    incounter_node* branch2;
    if (port == nullptr) {
      branch1 = new incounter_node;
      branch2 = nullptr;
    } else {
      branch1 = new incounter_node;
      branch2 = new incounter_node;
      branch1->parent = port;
      branch2->parent = port;
    }
    return std::make_pair(branch1, branch2);
  }
  
  incounter_node* increment() const {
    return increment((incounter_node*)nullptr).first;
  }
  
  status_type decrement(incounter_node* port) const {
    assert(port != nullptr);
    incounter_node* current = port;
    incounter_node* next = current->parent;
    while (next != nullptr) {
      delete current;
      while (true) {
        if (next->nb_removed_children.load() != 0) {
          break;
        }
        int orig = 0;
        if (next->nb_removed_children.compare_exchange_strong(orig, 1)) {
          return not_activated;
        }
      }
      current = next;
      next = next->parent;
    }
    assert(current != nullptr);
    assert(next == nullptr);
    delete current;
    return activated;
  }
  
  void check(pasl::sched::thread_p t) {
    assert(false);
  }
  
  void delta(pasl::sched::thread_p t, int64_t d) {
    assert(false);
  }
  
};

class outset : public pasl::sched::outstrategy::common {
public:
  
  using insert_status_type = enum {
    insert_success,
    insert_fail
  };
  
  using insert_result_type = std::pair<insert_status_type, outset_node*>;
  
  static constexpr int frozen_tag = 1;
  
  outset_node* root;
  
  node* n;
  
  outset(node* n)
  : n(n) {
    root = new outset_node;
  }
  
  ~outset() {
    deallocate_outset_tree(root);
  }
  
  outset_node* find_leaf() const {
    outset_node* current = root;
    while (true) {
      int i;
      for (i = 0; i < 2; i++) {
        if (current->children[i].load() != nullptr) {
          break;
        }
      }
      if (i == 2) {
        return current;
      } else {
        current = current->children[i].load();
      }
    }
  }
  
  bool is_finished() const {
    int tag = tagged_tag_of(root->children[0].load());
    return tag == frozen_tag;
  }
  
  insert_result_type insert(outset_node* outport, node* target, incounter_node* inport) {
    if (is_finished()) {
      return std::make_pair(insert_fail, nullptr);
    }
    outset_node* next = new outset_node;
    next->target = target;
    next->port = inport;
    outset_node* orig = nullptr;
    if (! (outport->children[0].compare_exchange_strong(orig, next))) {
      delete next;
      return std::make_pair(insert_fail, nullptr);
    }
    return std::make_pair(insert_success, next);
  }
  
  std::pair<outset_node*, outset_node*> fork2(outset_node* port) const {
    assert(port != nullptr);
    outset_node* branches[2];
    for (int i = 1; i >= 0; i--) {
      branches[i] = new outset_node;
      outset_node* orig = nullptr;
      if (! port->children[i].compare_exchange_strong(orig, branches[i])) {
        delete branches[i];
        return std::make_pair(nullptr, nullptr);
      }
    }
    return std::make_pair(branches[0], branches[1]);
  }
  
  void add(pasl::sched::thread_p t) {
    assert(false);
  }
  
  void finished() {
    if (n != nullptr) {
      decrement_inports(n);
    }
    notify_outset_tree_nodes(this);
  }
  
  bool should_deallocate_automatically = true;
  
  void enable_future() {
    should_deallocate_automatically = false;
  }
  
  void set_node(node* _n) {
    assert(n == nullptr);
    assert(_n != nullptr);
    n = _n;
  }
  
};
  
class node : public pasl::sched::thread {
public:
  
  using incounter_type = incounter;
  using outset_type = outset;
  
  const int uninitialized_block_id = -1;
  const int entry_block_id = 0;
  
  int current_block_id;
  
private:
  
  int continuation_block_id;
  
public:
  
  port_passing_mode inport_mode = port_passing_default;
  port_passing_mode outport_mode = port_passing_default;
  
  void set_inport_mode(port_passing_mode mode) {
    inport_mode = mode;
  }
  
  void set_outport_mode(port_passing_mode mode) {
    outport_mode = mode;
  }
  
  inport_map_type inports;
  outport_map_type outports;
  
  node()
  : current_block_id(uninitialized_block_id),
    continuation_block_id(entry_block_id) {
  }
  
  virtual void body() = 0;
  
  void run() {
    current_block_id = continuation_block_id;
    continuation_block_id = uninitialized_block_id;
    assert(current_block_id != uninitialized_block_id);
    body();
  }
  
  void decrement_inports() {
    for (auto p : inports) {
      decrement_incounter(p.first->n, p.first, p.second);
    }
    inports.clear();
  }
  
  void prepare_for_transfer(int target) {
    pasl::sched::threaddag::reuse_calling_thread();
    continuation_block_id = target;
  }
  
  void jump_to(int continuation_block_id) {
    prepare_for_transfer(continuation_block_id);
    continue_with(this);
  }
  
  void async(node* producer, node* consumer, int continuation_block_id) {
    prepare_node(producer, incounter_ready(), outset_unary(producer));
    node* caller = this;
    insert_inport(producer, (incounter*)consumer->in, (incounter_node*)nullptr);
    propagate_ports_for(caller, producer);
    caller->jump_to(continuation_block_id);
    add_node(producer);
  }
  
  void finish(node* producer, int continuation_block_id) {
    prepare_node(producer, incounter_ready(), outset_unary(producer));
    node* consumer = this;
    join_with(consumer, new incounter(consumer));
    propagate_ports_for(consumer, producer);
    incounter_node* consumer_inport = increment_incounter(consumer);
    insert_inport(producer, consumer, consumer_inport);
    consumer->prepare_for_transfer(continuation_block_id);
    add_node(producer);
  }
  
  static outset* allocate_future() {
    outset* out = outset_new(nullptr);
    out->enable_future();
    return out;
  }
  
  void listen_on(outset* out) {
    node* caller = this;
    insert_outport(caller, out, out->find_leaf());
  }
  
  void future(node* producer, outset* producer_out, int continuation_block_id) {
    prepare_node(producer, incounter_ready(), producer_out);
    producer_out->set_node(producer);
    node* caller = this;
    propagate_ports_for(caller, producer);
    listen_on(producer_out);
    caller->jump_to(continuation_block_id);
    add_node(producer);
  }
  
  outset* future(node* producer, int continuation_block_id) {
    outset* producer_out = allocate_future();
    future(producer, producer_out, continuation_block_id);
    return producer_out;
  }
  
  void force(outset* producer_out, int continuation_block_id) {
    node* consumer = this;
    prepare_for_transfer(continuation_block_id);
    join_with(consumer, incounter_unary());
    outset::insert_result_type insert_result;
    if (producer_out->is_finished()) {
      insert_result.first = outset::insert_fail;
    } else {
      outset_node* source_outport = find_outport(consumer, producer_out);
      insert_result = producer_out->insert(source_outport, consumer, (incounter_node*)nullptr);
    }
    outset::insert_status_type insert_status = insert_result.first;
    if (insert_status == outset::insert_success) {
      outset_node* producer_outport = insert_result.second;
      insert_outport(consumer, producer_out, producer_outport);
    } else if (insert_status == outset::insert_fail) {
      add_node(consumer);
    } else {
      assert(false);
    }
    consumer->outports.erase(producer_out);
  }
  
  void deallocate_future(outset* future) {
    portpassing::deallocate_future(this, future);
  }
  
  template <class Body>
  void parallel_for(long lo, long hi, const Body& body, int continuation_block_id) {
    node* consumer = this;
    node* producer = new_parallel_for(lo, hi, consumer, body);
    prepare_node(producer, incounter_ready(), outset_unary(producer));
    join_with(consumer, new incounter(consumer));
    propagate_ports_for(consumer, producer);
    incounter_node* consumer_inport = increment_incounter(consumer);
    insert_inport(producer, consumer, consumer_inport);
    consumer->prepare_for_transfer(continuation_block_id);
    add_node(producer);
  }
  
  void split_with(node* new_sibling) {
    node* caller = this;
    prepare_node(new_sibling);
    propagate_ports_for(caller, new_sibling);
  }
  
  void call(node* target, int continuation_block_id) {
    finish(target, continuation_block_id);
  }
  
  void detach(int continuation_block_id) {
    prepare_for_transfer(continuation_block_id);
    join_with(this, incounter_ready());
  }
  
  THREAD_COST_UNKNOWN
  
};
  
void prepare_node(node* n) {
  prepare_node(n, incounter_new(n), outset_new(n));
}

void prepare_node(node* n, incounter* in) {
  prepare_node(n, in, outset_new(n));
}

void prepare_node(node* n, outset* out) {
  prepare_node(n, incounter_new(n), out);
}

void prepare_node(node* n, incounter* in, outset* out) {
  n->set_instrategy(in);
  n->set_outstrategy(out);
}
  
incounter* incounter_ready() {
  return (incounter*)pasl::sched::instrategy::ready_new();
}

incounter* incounter_unary() {
  return (incounter*)pasl::sched::instrategy::unary_new();
}

incounter* incounter_fetch_add() {
  return (incounter*)pasl::sched::instrategy::fetch_add_new();
}
  
incounter* incounter_new(node* n) {
  return new incounter(n);
}

outset* outset_unary(node* n) {
  return (outset*)pasl::sched::outstrategy::portpassing_unary_new((pasl::sched::thread_p)n);
}

outset* outset_noop() {
  return (outset*)pasl::sched::outstrategy::noop_new();
}
  
outset* outset_new(node* n) {
  return new outset(n);
}
  
void insert_inport(node* caller, incounter* target_in, incounter_node* target_inport) {
  caller->inports.insert(std::make_pair(target_in, target_inport));
}
  
void insert_inport(node* caller, node* target, incounter_node* target_inport) {
  insert_inport(caller, (incounter*)target->in, target_inport);
}

void insert_outport(node* caller, outset* target_out, outset_node* target_outport) {
  assert(target_outport != nullptr);
  caller->outports.insert(std::make_pair(target_out, target_outport));
}
  
void insert_outport(node* caller, node* target, outset_node* target_outport) {
  insert_outport(caller, (outset*)target->out, target_outport);
}

incounter_node* find_inport(node* caller, incounter* target_in) {
  auto target_inport_result = caller->inports.find(target_in);
  assert(target_inport_result != caller->inports.end());
  return target_inport_result->second;
}

outset_node* find_outport(node* caller, outset* target_out) {
  auto target_outport_result = caller->outports.find(target_out);
  assert(target_outport_result != caller->outports.end());
  return target_outport_result->second;
}
  
template <class Map>
void intersect_with(const Map& source, Map& destination) {
  Map result;
  for (auto item : source) {
    if (destination.find(item.first) != destination.cend()) {
      result.insert(item);
    }
  }
  result.swap(destination);
}
  
template <class Map>
void difference_with(const Map& source, Map& destination) {
  Map result;
  for (auto item : source) {
    if (destination.find(item.first) == destination.cend()) {
      result.insert(item);
    }
  }
  result.swap(destination);
}
  
template <class Map>
void propagate_ports_for(port_passing_mode mode, const Map& parent_ports, Map& child_ports) {
  switch (mode) {
    case port_passing_default: {
      child_ports = parent_ports;
      break;
    }
    case port_passing_intersection: {
      intersect_with(parent_ports, child_ports);
      break;
    }
    case port_passing_difference: {
      difference_with(parent_ports, child_ports);
      break;
    }
  }
}
  
void fork_in_ports_for(inport_map_type& parent_ports, inport_map_type& child_ports) {
  for (auto item : parent_ports) {
    if (child_ports.find(item.first) != child_ports.cend()) {
      incounter* in = item.first;
      incounter_node* port = item.second;
      auto new_ports = in->increment(port);
      parent_ports[in] = new_ports.first;
      child_ports[in] = new_ports.second;
    }
  }
}

void fork_out_ports_for(outport_map_type& parent_ports, outport_map_type& child_ports) {
  std::vector<outset*> to_erase;
  for (auto item : parent_ports) {
    if (child_ports.find(item.first) != child_ports.cend()) {
      outset* out = item.first;
      outset_node* port = item.second;
      auto new_ports = out->fork2(port);
      if (new_ports.first == nullptr) {
        to_erase.push_back(out);
      } else {
        parent_ports[out] = new_ports.first;
        child_ports[out] = new_ports.second;
      }
    }
  }
  for (outset* out : to_erase) {
    parent_ports.erase(out);
    child_ports.erase(out);
  }
}
  
void propagate_ports_for(node* parent, node* child) {
  port_passing_mode in_port_mode = child->inport_mode;
  port_passing_mode out_port_mode = child->outport_mode;
  propagate_ports_for(in_port_mode, parent->inports, child->inports);
  fork_in_ports_for(parent->inports, child->inports);
  propagate_ports_for(out_port_mode, parent->outports, child->outports);
  fork_out_ports_for(parent->outports, child->outports);
}

incounter_node* increment_incounter(node* n) {
  incounter* in = (incounter*)n->in;
  return in->increment();
}
  
std::pair<incounter_node*, incounter_node*> increment_incounter(node* n, incounter_node* n_port) {
  incounter* n_in = (incounter*)n->in;
  long tag = pasl::sched::instrategy::extract_tag(n_in);
  assert(tag != pasl::sched::instrategy::READY_TAG);
  if (tag == pasl::sched::instrategy::UNARY_TAG) {
    // nothing to do
    return std::make_pair(nullptr, nullptr);
  } else if (tag == pasl::sched::instrategy::FETCH_ADD_TAG) {
    pasl::data::tagged::atomic_fetch_and_add<pasl::sched::instrategy_p>(&(n->in), 1l);
    return std::make_pair(nullptr, nullptr);
  } else {
    incounter* in = (incounter*)n->in;
    return in->increment(n_port);
  }
}
  
std::pair<incounter_node*, incounter_node*> increment_incounter(node* caller, node* target) {
  incounter_node* target_inport = find_inport(caller, (incounter*)target->in);
  return increment_incounter(target, target_inport);
}

void decrement_incounter(node* n, incounter* n_in, incounter_node* n_port) {
  long tag = pasl::sched::instrategy::extract_tag(n_in);
  assert(tag != pasl::sched::instrategy::READY_TAG);
  if (tag == pasl::sched::instrategy::UNARY_TAG) {
    pasl::sched::instrategy::schedule(n);
  } else if (tag == pasl::sched::instrategy::FETCH_ADD_TAG) {
    int64_t old = pasl::data::tagged::atomic_fetch_and_add<pasl::sched::instrategy_p>(&(n->in), -1l);
    if (old == 1) {
      pasl::sched::instrategy::schedule(n);
    }
  } else {
    incounter::status_type status = n_in->decrement(n_port);
    if (status == incounter::activated) {
      n_in->start(n);
    }
  }
}
  
void decrement_incounter(node* n, incounter_node* n_port) {
  decrement_incounter(n, (incounter*)n->in, n_port);
}
  
void decrement_inports(node* n) {
  n->decrement_inports();
}
  
void add_node(node* n) {
  incounter* n_in = (incounter*)n->in;
  long tag = pasl::sched::instrategy::extract_tag(n_in);
  if (   (tag == pasl::sched::instrategy::UNARY_TAG)
      || (tag == pasl::sched::instrategy::READY_TAG)) {
    // nothing to do
  } else if (tag == pasl::sched::instrategy::FETCH_ADD_TAG) {
    // nothing to do
  } else {
    delete n->in;
  }
  pasl::sched::instrategy::schedule(n);
}
  
outset* capture_outset() {
  auto sched = pasl::sched::threaddag::my_sched();
  outset* out = (outset*)sched->get_outstrategy();
  assert(out != nullptr);
  sched->set_outstrategy(outset_noop());
  return out;
}
  
void join_with(node* n, incounter* in) {
  prepare_node(n, in, capture_outset());
}

void continue_with(node* n) {
  join_with(n, incounter_ready());
  add_node(n);
}
  
void portpassing_finished(pasl::sched::thread_p t) {
  node* n = tagged_pointer_of((node*)t);
  n->decrement_inports();
}
  
void deallocate_future(node* caller, outset* future) {
  assert(! future->should_deallocate_automatically);
  caller->outports.erase(future);
  delete future;
}
  
template <class Body>
class lazy_parallel_for_rec : public node {
public:
  
  long lo;
  long hi;
  node* join;
  Body _body;
  
  lazy_parallel_for_rec(long lo, long hi, node* join, const Body& _body)
  : lo(lo), hi(hi), join(join), _body(_body) { }
  
  enum {
    process_block,
    repeat_block,
    exit
  };
  
  void body() {
    switch (node::current_block_id) {
      case process_block: {
        long n = std::min(hi, lo + communication_delay);
        long i;
        for (i = lo; i < n; i++) {
          _body(i);
        }
        lo = i;
        node::jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (lo < hi) {
          node::jump_to(process_block);
        }
        break;
      }
    }
  }
  
  size_t size() const {
    return hi - lo;
  }
  
  pasl::sched::thread_p split() {
    node* consumer = join;
    node* caller = this;
    int mid = (hi + lo) / 2;
    lazy_parallel_for_rec producer = new lazy_parallel_for_rec(mid, hi, join, _body);
    hi = mid;
    prepare_node(producer);
    insert_inport(producer, (incounter*)consumer->in, (incounter_node*)nullptr);
    propagate_ports_for(caller, producer);
    return producer;
  }
  
};

template <class Body>
node* new_parallel_for(long lo, long hi, node* join, const Body& body) {
  return new lazy_parallel_for_rec<Body>(lo, hi, join, body);
}
  
void notify_outset_tree_nodes_partial(std::deque<outset_node*>& todo) {
  int k = 0;
  while ( (k < communication_delay) && (! todo.empty()) ) {
    outset_node* n = todo.back();
    todo.pop_back();
    if (n->target != nullptr) {
      decrement_incounter(n->target, n->port);
    }
    for (int i = 0; i < 2; i++) {
      outset_node* orig;
      while (true) {
        orig = n->children[i].load();
        outset_node* next = tagged_tag_with(orig, outset::frozen_tag);
        if (n->children[i].compare_exchange_strong(orig, next)) {
          break;
        }
      }
      if (orig != nullptr) {
        todo.push_back(orig);
      }
    }
    k++;
  }
}
  
class notify_outset_tree_nodes_par_rec : public node {
public:
  
  using self_type = notify_outset_tree_nodes_par_rec;
  
  enum {
    process_block=0,
    repeat_block,
    exit_block
  };
  
  node* join;
  std::deque<outset_node*> todo;
  
  notify_outset_tree_nodes_par_rec(node* join, outset_node* n)
  : join(join) {
    todo.push_back(n);
  }
  
  notify_outset_tree_nodes_par_rec(node* join, std::deque<outset_node*>& _todo)
  : join(join) {
    _todo.swap(todo);
  }
  
  void body() {
    switch (current_block_id) {
      case process_block: {
        notify_outset_tree_nodes_partial(todo);
        jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (! todo.empty()) {
          jump_to(process_block);
        }
        break;
      }
      default:
        break;
    }
  }
  
  size_t size() const {
    return todo.size();
  }
  
  pasl::sched::thread_p split() {
    assert(size() >= 2);
    auto n = todo.front();
    todo.pop_front();
    node* consumer = join;
    node* caller = this;
    auto producer = new self_type(join, n);
    prepare_node(producer);
    insert_inport(producer, (incounter*)consumer->in, (incounter_node*)nullptr);
    propagate_ports_for(caller, producer);
    return producer;
  }
  
};
  
class notify_outset_tree_nodes_par : public node {
public:
  
  using self_type = notify_outset_tree_nodes_par;
  
  enum {
    entry_block=0,
    exit_block
  };
  
  outset* out;
  std::deque<outset_node*> todo;
  
  notify_outset_tree_nodes_par(outset* out, std::deque<outset_node*>& _todo)
  : out(out) {
    todo.swap(_todo);
  }
  
  void body() {
    switch (current_block_id) {
      case entry_block: {
        finish(new notify_outset_tree_nodes_par_rec(this, todo),
               exit_block);
        break;
      }
      case exit_block: {
        if (out->should_deallocate_automatically) {
          delete out;
        }
        break;
      }
      default:
        break;
    }
  }
  
};
  
void notify_outset_tree_nodes(outset* out) {
  std::deque<outset_node*> todo;
  todo.push_back(out->root);
  notify_outset_tree_nodes_partial(todo);
  if (! todo.empty()) {
    auto n = new notify_outset_tree_nodes_par(out, todo);
    prepare_node(n);
    add_node(n);
  } else {
    if (out->should_deallocate_automatically) {
      delete out;
    }
  }
}
  
void deallocate_outset_tree_partial(std::deque<outset_node*>& todo) {
  int k = 0;
  while ( (k < communication_delay) && (! todo.empty()) ) {
    outset_node* n = todo.back();
    todo.pop_back();
    for (int i = 0; i < 2; i++) {
      outset_node* child = tagged_pointer_of(n->children[i].load());
      if (child != nullptr) {
        todo.push_back(child);
      }
    }
    delete n;
    k++;
  }
}

class deallocate_outset_tree_par : public node {
public:
  
  using self_type = deallocate_outset_tree_par;
  
  enum {
    process_block=0,
    repeat_block
  };
  
  std::deque<outset_node*> todo;
  
  void body() {
    switch (current_block_id) {
      case process_block: {
        deallocate_outset_tree_partial(todo);
        jump_to(repeat_block);
        break;
      }
      case repeat_block: {
        if (! todo.empty()) {
          jump_to(process_block);
        }
        break;
      }
      default:
        break;
    }
  }
  
  size_t size() const {
    return todo.size();
  }
  
  pasl::sched::thread_p split() {
    assert(size() >= 2);
    auto n = todo.front();
    todo.pop_front();
    auto t = new self_type();
    prepare_node(t);
    t->todo.push_back(n);
    return t;
  }
  
};
  
void deallocate_outset_tree(outset_node* root) {
  deallocate_outset_tree_par d;
  d.todo.push_back(root);
  deallocate_outset_tree_partial(d.todo);
  if (! d.todo.empty()) {
    node* n = new deallocate_outset_tree_par(d);
    prepare_node(n);
    add_node(n);
  }
}
  
} // end namespace

/*---------------------------------------------------------------------*/


/*---------------------------------------------------------------------*/
/* Benchmarks */

namespace benchmarks {
  
template <class node>
using outset_of = typename node::outset_type;
  
unsigned int hashu(unsigned int a) {
  a = (a+0x7ed55d16) + (a<<12);
  a = (a^0xc761c23c) ^ (a>>19);
  a = (a+0x165667b1) + (a<<5);
  a = (a+0xd3a2646c) ^ (a<<9);
  a = (a+0xfd7046c5) + (a<<3);
  a = (a^0xb55a4f09) ^ (a>>16);
  return a;
}

template <class Incounter>
void benchmark_incounter_thread(int my_id, Incounter& incounter, bool& should_stop, int& counter, unsigned int seed) {
  int c = 0;
  unsigned int rng = seed;
  int nb_pending_increments = 0;
  while (! should_stop) {
    if (nb_pending_increments > 0 && rng % 2 == 0) {
      incounter.decrement(my_id);
    } else {
      nb_pending_increments++;
      incounter.increment(my_id);
    }
    rng = hashu(rng);
    c++;
  }
  while (nb_pending_increments > 0) {
    incounter.decrement(my_id);
    nb_pending_increments--;
    c++;
  }
  counter = c;
}
  
class simple_incounter_wrapper {
public:
  
  std::atomic<int> counter;
  
  simple_incounter_wrapper() {
    counter.store(0);
  }
  
  void increment(int) {
    counter++;
  }
  
  bool decrement(int) {
    return counter-- == 0;
  }
  
};

class snzi_incounter_wrapper {
public:
  
  pasl::data::snzi::tree snzi;
  
  snzi_incounter_wrapper(int branching_factor, int nb_levels)
  : snzi(branching_factor, nb_levels) { }
  
  int my_leaf_node(int hash) const {
    return abs(hash) % snzi.get_nb_leaf_nodes();
  }
  
  void increment(int hash) {
    int i = my_leaf_node(hash);
    snzi.ith_leaf_node(i)->arrive();
  }
  
  bool decrement(int hash) {
    int i = my_leaf_node(hash);
    return snzi.ith_leaf_node(i)->depart();
  }
  
};

class dyntree_incounter_wrapper {
public:
  
  direct::dyntree::dyntree_incounter incounter;
  
  dyntree_incounter_wrapper() { }
  
  void increment(int) {
    incounter.increment(nullptr);
  }
  
  bool decrement(int) {
    return incounter.decrement(nullptr);
  }
  
};

template <class Outset>
void benchmark_outset_thread(Outset& outset, bool& should_stop, int& counter) {
  int c = 0;
  while (! should_stop) {
    outset.add(nullptr);
    c++;
  }
  counter = c;
}

template <class Benchmark>
void launch_microbenchmark(const Benchmark& benchmark, int nb_threads, int nb_milliseconds) {
  bool should_stop = false;
  std::vector<std::thread*> threads;
  int counters[nb_threads];
  for (int i = 0; i < nb_threads; i++) {
    int& counter = counters[i];
    counter = 0;
    threads.push_back(new std::thread([&] {
      benchmark(i, should_stop, counter);
    }));
  }
  auto start = std::chrono::high_resolution_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(nb_milliseconds));
  should_stop = true;
  for (std::thread* thread : threads) {
    thread->join();
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> diff = end-start;
  std::cout << "exectime\t" << diff.count() << std::endl;
  int count = 0;
  for (int i = 0; i < nb_threads; i++) {
    count += counters[i];
  }
  std::cout << "nb_operations  " << count << std::endl;
  for (std::thread* t : threads) {
    delete t;
  }
}

void launch_outset_microbenchmark() {
  int nb_threads = pasl::util::cmdline::parse_int("proc");
  int nb_milliseconds = pasl::util::cmdline::parse_int("nb_milliseconds");
  direct::simple::simple_outset* simple_outset = nullptr;
  direct::dyntree::dyntree_outset* dyntree_outset = nullptr;
  pasl::util::cmdline::argmap_dispatch c;
  c.add("simple", [&] {
    simple_outset = new direct::simple::simple_outset;
  });
  c.add("dyntree", [&] {
    dyntree_outset = new direct::dyntree::dyntree_outset;
  });
  c.find_by_arg("outset")();
  auto benchmark_thread = [&] (int, bool& should_stop, int& counter) {
    if (simple_outset != nullptr) {
      benchmark_outset_thread(*simple_outset, should_stop, counter);
    } else if (dyntree_outset != nullptr) {
      benchmark_outset_thread(*dyntree_outset, should_stop, counter);
    }
  };
  launch_microbenchmark(benchmark_thread, nb_threads, nb_milliseconds);
  if (simple_outset != nullptr) {
    delete simple_outset;
  } else if (dyntree_outset != nullptr) {
    delete dyntree_outset;
  }
}

void launch_incounter_microbenchmark() {
  int seed = pasl::util::cmdline::parse_int("seed");
  int nb_threads = pasl::util::cmdline::parse_int("proc");
  int nb_milliseconds = pasl::util::cmdline::parse_int("nb_milliseconds");
  simple_incounter_wrapper* simple_incounter = nullptr;
  snzi_incounter_wrapper* snzi_incounter = nullptr;
  dyntree_incounter_wrapper* dyntree_incounter = nullptr;
  pasl::util::cmdline::argmap_dispatch c;
  c.add("simple", [&] {
    simple_incounter = new simple_incounter_wrapper;
  });
  c.add("snzi", [&] {
    int branching_factor = pasl::util::cmdline::parse_int("branching_factor");
    int nb_levels = pasl::util::cmdline::parse_int("nb_levels");
    snzi_incounter = new snzi_incounter_wrapper(branching_factor, nb_levels);
  });
  c.add("dyntree", [&] {
    dyntree_incounter = new dyntree_incounter_wrapper;
  });
  c.find_by_arg("incounter")();
  auto benchmark_thread = [&] (int my_id, bool& should_stop, int& counter) {
    if (simple_incounter != nullptr) {
      benchmark_incounter_thread(my_id, *simple_incounter, should_stop, counter, seed);
    } else if (snzi_incounter != nullptr) {
      benchmark_incounter_thread(my_id, *snzi_incounter, should_stop, counter, seed);
    } else if (dyntree_incounter != nullptr) {
      benchmark_incounter_thread(my_id, *dyntree_incounter, should_stop, counter, seed);
    } else {
      assert(false);
    }
  };
  launch_microbenchmark(benchmark_thread, nb_threads, nb_milliseconds);
  if (simple_incounter == nullptr) {
    delete simple_incounter;
  } else if (snzi_incounter != nullptr) {
    delete snzi_incounter;
  } else if (dyntree_incounter != nullptr) {
    delete dyntree_incounter;
  }
}

bool should_async_microbench_terminate = false;
  
pasl::data::perworker::counter::carray<int> async_microbench_counter;

template <class node>
class async_microbench_loop : public node {
public:
  
  enum {
    entry,
    exit
  };
  
  node* join;
  
  async_microbench_loop(node* join)
  : join(join) { }
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        if (! should_async_microbench_terminate) {
          async_microbench_counter++;
          node::async(new async_microbench_loop(join), join,
                      exit);
        }
        break;
      }
      case exit: {
        node::jump_to(entry);
        break;
      }
    }
  }
  
};
  
template <class node>
class async_microbench : public node {
public:
  
  enum {
    entry,
    exit
  };
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        async_microbench_counter.init(0);
        node::finish(new async_microbench_loop<node>(this),
                     exit);
        break;
      }
      case exit: {
        std::cout << "nb_asyncs  " << async_microbench_counter.sum() << std::endl;
        break;
      }
    }
  }
  
};
  
pasl::data::perworker::counter::carray<int> edge_throughput_microbench_counter;
  
template <class node>
class edge_throughput_microbench_force : public node {
public:
  
  enum {
    entry,
    exit
  };

  edge_throughput_microbench_force(outset_of<node>* producer)
  : producer(producer) { }
  
  outset_of<node>* producer;
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        edge_throughput_microbench_counter++;
        node::force(producer,
                    exit);
        break;
      }
      case exit: {
        break;
      }
    }
  }
  
};
  
template <class node>
class edge_throughput_microbench_loop : public node {
public:
  
  enum {
    entry,
    recurse,
    loop,
    exit
  };
  
  edge_throughput_microbench_loop(node* join,
                                  outset_of<node>* producer,
                                  std::atomic<node*>* buffer)
  : join(join), producer(producer), buffer(buffer) { }
  
  node* join;
  outset_of<node>* producer;
  std::atomic<node*>* buffer;
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        node::async(new edge_throughput_microbench_force<node>(producer), join,
                    recurse);
        break;
      }
      case recurse: {
        node::async(new edge_throughput_microbench_loop(join, producer, buffer), join,
                    loop);
        break;
      }
      case loop: {
        node* orig = buffer->load();
        if (orig == nullptr) {
          node::jump_to(entry);
        } else if (orig == tagged_tag_with((node*)nullptr, 1)) {
          break;
        } else {
          node* next = tagged_tag_with((node*)nullptr, 1);
          if (buffer->compare_exchange_strong(orig, next)) {
            node::call(orig,
                       exit);
          }
        }
        break;
      }
      case exit: {
        break;
      }
    }
  }
  
};
  
template <class node>
class edge_throughput_microbench_future : public node {
public:
  
  enum {
    entry,
    exit
  };
  
  edge_throughput_microbench_future(int nb_milliseconds, std::atomic<node*>* buffer)
  : nb_milliseconds(nb_milliseconds), buffer(buffer) { }
  
  std::atomic<node*>* buffer;
  int nb_milliseconds;
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        std::thread timer([&] {
          std::this_thread::sleep_for(std::chrono::milliseconds(nb_milliseconds));
          buffer->store(this);
        });
        timer.detach();
        node::detach(exit);
        break;
      }
      case exit: {
        break;
      }
    }
  }
  
};
  
template <class node>
class edge_throughput_microbench : public node {
public:
  
  enum {
    entry,
    gen,
    exit
  };
  
  edge_throughput_microbench(int nb_milliseconds)
  : nb_milliseconds(nb_milliseconds) { }
  
  std::atomic<node*> buffer;
  int nb_milliseconds;
  outset_of<node>* producer;
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        edge_throughput_microbench_counter.init(0);
        producer = node::future(new edge_throughput_microbench_future<node>(nb_milliseconds, &buffer),
                                gen);
        break;
      }
      case gen: {
        node::finish(new edge_throughput_microbench_loop<node>(this, producer, &buffer),
                     exit);
        break;
      }
      case exit: {
        std::cout << "nb_forces  " << edge_throughput_microbench_counter.sum() << std::endl;
        break;
      }
    }
  }
  
};
  
std::atomic<int> async_leaf_counter;
std::atomic<int> async_interior_counter;

template <class node>
class async_bintree_rec : public node {
public:
  
  enum {
    async_bintree_rec_entry,
    async_bintree_rec_mid,
    async_bintree_rec_exit
  };
  
  int lo;
  int hi;
  node* consumer;
  
  int mid;
  
  async_bintree_rec(int lo, int hi, node* consumer)
  : lo(lo), hi(hi), consumer(consumer) { }
  
  void body() {
    switch (node::current_block_id) {
      case async_bintree_rec_entry: {
        int n = hi - lo;
        if (n == 0) {
          return;
        } else if (n == 1) {
          async_leaf_counter.fetch_add(1);
        } else {
          async_interior_counter.fetch_add(1);
          mid = (lo + hi) / 2;
          node::async(new async_bintree_rec(lo, mid, consumer), consumer,
                      async_bintree_rec_mid);
        }
        break;
      }
      case async_bintree_rec_mid: {
        node::async(new async_bintree_rec(mid, hi, consumer), consumer,
                    async_bintree_rec_exit);
        break;
      }
      case async_bintree_rec_exit: {
        break;
      }
      default:
        break;
    }
  }
  
};

template <class node>
class async_bintree : public node {
public:
  
  enum {
    async_bintree_entry,
    async_bintree_exit
  };
  
  int n;
  
  async_bintree(int n)
  : n(n) { }
  
  void body() {
    switch (node::current_block_id) {
      case async_bintree_entry: {
        async_leaf_counter.store(0);
        async_interior_counter.store(0);
        node::finish(new async_bintree_rec<node>(0, n, this),
                     async_bintree_exit);
        break;
      }
      case async_bintree_exit: {
        assert(async_leaf_counter.load() == n);
        assert(async_interior_counter.load() + 1 == n);
        break;
      }
      default:
        break;
    }
  }
  
};

std::atomic<int> future_leaf_counter;
std::atomic<int> future_interior_counter;

template <class node>
class future_bintree_rec : public node {
public:
  
  enum {
    future_bintree_entry,
    future_bintree_branch2,
    future_bintree_force1,
    future_bintree_force2,
    future_bintree_exit
  };
  
  int lo;
  int hi;
  
  outset_of<node>* branch1_out;
  outset_of<node>* branch2_out;
  
  int mid;
  
  future_bintree_rec(int lo, int hi)
  : lo(lo), hi(hi) { }
  
  void body() {
    switch (node::current_block_id) {
      case future_bintree_entry: {
        int n = hi - lo;
        if (n == 0) {
          return;
        } else if (n == 1) {
          future_leaf_counter.fetch_add(1);
        } else {
          mid = (lo + hi) / 2;
          branch1_out = node::future(new future_bintree_rec<node>(lo, mid),
                                     future_bintree_branch2);
        }
        break;
      }
      case future_bintree_branch2: {
        branch2_out = node::future(new future_bintree_rec<node>(mid, hi),
                                   future_bintree_force1);
        break;
      }
      case future_bintree_force1: {
        node::force(branch1_out,
                    future_bintree_force2);
        break;
      }
      case future_bintree_force2: {
        node::force(branch2_out,
                    future_bintree_exit);
        break;
      }
      case future_bintree_exit: {
        future_interior_counter.fetch_add(1);
        node::deallocate_future(branch1_out);
        node::deallocate_future(branch2_out);
        break;
      }
      default:
        break;
    }
  }
  
};

template <class node>
class future_bintree : public node {
public:
  
  enum {
    future_bintree_entry,
    future_bintree_force,
    future_bintree_exit
  };
  
  int n;
  
  outset_of<node>* root_out;
  
  future_bintree(int n)
  : n(n) { }
  
  void body() {
    switch (node::current_block_id) {
      case future_bintree_entry: {
        future_leaf_counter.store(0);
        future_interior_counter.store(0);
        root_out = node::future(new future_bintree_rec<node>(0, n),
                                future_bintree_force);
        break;
      }
      case future_bintree_force: {
        node::force(root_out,
                    future_bintree_exit);
        break;
      }
      case future_bintree_exit: {
        node::deallocate_future(root_out);
        assert(future_leaf_counter.load() == n);
        assert(future_interior_counter.load() + 1 == n);
        break;
      }
      default:
        break;
    }
  }
  
};
  
template <class node>
class parallel_for_test : public node {
public:
  
  long n;
  int* array;
  
  enum {
    entry,
    exit
  };
  
  parallel_for_test(long n)
  : n(n) { }
  
  bool check() {
    for (long i = 0; i < n; i++) {
      if (array[i] != i) {
        return false;
      }
    }
    return true;
  }
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        array = (int*)malloc(n* sizeof(int));
        node::parallel_for(0, n, [&] (long i) {
          array[i] = (int)i;
        }, exit);
        break;
      }
      case exit: {
        assert(check());
        free(array);
        break;
      }
    }
  }
  
};

template <class Body_gen, class node>
class eager_parallel_for_rec : public node {
public:
  
  enum {
    parallel_for_rec_entry,
    parallel_for_rec_branch2,
    parallel_for_rec_exit
  };
  
  int lo;
  int hi;
  Body_gen body_gen;
  node* join;
  
  int mid;
  
  eager_parallel_for_rec(int lo, int hi, Body_gen body_gen, node* join)
  : lo(lo), hi(hi), body_gen(body_gen), join(join) { }
  
  void body() {
    switch (node::current_block_id) {
      case parallel_for_rec_entry: {
        int n = hi - lo;
        if (n == 0) {
          // nothing to do
        } else if (n == 1) {
          node::call(body_gen(lo),
                     parallel_for_rec_exit);
        } else {
          mid = (hi + lo) / 2;
          node::async(new eager_parallel_for_rec(lo, mid, body_gen, join), join,
                      parallel_for_rec_branch2);
        }
        break;
      }
      case parallel_for_rec_branch2: {
        node::async(new eager_parallel_for_rec(mid, hi, body_gen, join), join,
                    parallel_for_rec_exit);
        break;
      }
      case parallel_for_rec_exit: {
        // nothing to do
        break;
      }
      default:
        break;
    }
  }
  
};

template <class Body_gen, class node>
class eager_parallel_for : public node {
public:
  
  enum {
    parallel_for_entry,
    parallel_for_exit
  };
  
  int lo;
  int hi;
  Body_gen body_gen;
  
  eager_parallel_for(int lo, int hi, Body_gen body_gen)
  : lo(lo), hi(hi), body_gen(body_gen) { }
  
  void body() {
    switch (node::current_block_id) {
      case parallel_for_entry: {
        node::finish(new eager_parallel_for_rec<Body_gen, node>(lo, hi, body_gen, this),
                     parallel_for_exit);
        break;
      }
      case parallel_for_exit: {
        // nothing to do
        break;
      }
      default:
        break;
    }
  }
  
};

std::atomic<int> future_pool_leaf_counter;
std::atomic<int> future_pool_interior_counter;

static long fib (long n){
  if (n < 2)
    return n;
  else
    return fib (n - 1) + fib (n - 2);
}

int fib_input = 22;

long fib_result;

template <class node>
class future_body : public node {
public:
  
  enum {
    future_body_entry,
    future_body_exit
  };
  
  void body() {
    switch (node::current_block_id) {
      case future_body_entry: {
        fib_result = fib(fib_input);
        break;
      }
      case future_body_exit: {
        break;
      }
      default:
        break;
    }
  }
  
};

std::atomic<int> future_pool_counter;

template <class node>
class future_reader : public node {
public:
  
  enum {
    future_reader_entry,
    future_reader_exit
  };
  
  outset_of<node>* f;
  
  int i;
  
  future_reader(outset_of<node>* f, int i)
  : f(f), i(i) { }
  
  void body() {
    switch (node::current_block_id) {
      case future_reader_entry: {
        node::force(f,
                    future_reader_exit);
        break;
      }
      case future_reader_exit: {
        future_pool_counter.fetch_add(1);
        assert(fib_result == fib(fib_input));
        break;
      }
      default:
        break;
    }
  }
  
};

template <class node>
class future_pool : public node {
public:
  
  enum {
    future_pool_entry,
    future_pool_call,
    future_pool_exit
  };
  
  int n;
  
  outset_of<node>* f;
  
  future_pool(int n)
  : n(n) { }
  
  void body() {
    switch (node::current_block_id) {
      case future_pool_entry: {
        f = node::future(new future_body<node>,
                         future_pool_call);
        break;
      }
      case future_pool_call: {
        auto loop_body = [=] (int i) {
          return new future_reader<node>(f, i);
        };
        node::call(new eager_parallel_for<decltype(loop_body), node>(0, n, loop_body),
                   future_pool_exit);
        break;
      }
      case future_pool_exit: {
        node::deallocate_future(f);
        assert(future_pool_counter.load() == n);
        break;
      }
      default:
        break;
    }
  }
  
};

int nb_levels(int n) {
  assert(n >= 1);
  return 2 * (n - 1) + 1;
}

int nb_cells_in_level(int n, int l) {
  assert((l >= 1) && (l <= nb_levels(n)));
  return (l <= n) ? l : (nb_levels(n) + 1) - l;
}

std::pair<int, int> index_of_cell_at_pos(int n, int l, int pos) {
  assert((pos >= 0) && (pos < nb_cells_in_level(n, l)));
  int i;
  int j;
  if (l <= n) { // either on or above the diagonal
    i = pos;
    j = l - (pos + 1);
  } else {      // below the diagonal
    i = (l - n) + pos;
    j = n - (pos + 1);
  }
  return std::make_pair(i, j);
}

int row_major_index_of(int n, int i, int j) {
  return i * n + j;
}

template <class Item>
Item& row_major_address_of(Item* items, int n, int i, int j) {
  assert(i >= 0 && i < n);
  assert(j >= 0 && j < n);
  return items[row_major_index_of(n, i, j)];
}

template <class Item>
class matrix_type {
public:
  
  using value_type = Item;
  
  class Deleter {
  public:
    void operator()(value_type* ptr) {
      free(ptr);
    }
  };
  
  std::unique_ptr<value_type[], Deleter> items;
  int n;
  
  void fill(value_type val) {
    for (int i = 0; i < n*n; i++) {
      items[i] = val;
    }
  }
  
  void init() {
    value_type* ptr = (value_type*)malloc(sizeof(value_type) * (n*n));
    assert(ptr != nullptr);
    items.reset(ptr);
  }
  
  matrix_type(int n, value_type val)
  : n(n) {
    init();
    fill(val);
  }
  
  matrix_type(int n)
  : n(n) {
    init();
  }
  
  value_type& subscript(std::pair<int, int> pos) const {
    return subscript(pos.first, pos.second);
  }
  
  value_type& subscript(int i, int j) const {
    return row_major_address_of(&items[0], n, i, j);
  }
  
};

template <class Item>
std::ostream& operator<<(std::ostream& out, const matrix_type<Item>& xs) {
  out << "{\n";
  for (int i = 0; i < xs.n; i++) {
    out << "{ ";
    for (int j = 0; j < xs.n; j++) {
      if (j+1 < xs.n) {
        out << xs.subscript(i, j) << ",\t";
      } else {
        out << xs.subscript(i, j);
      }
    }
    out << " }\n";
  }
  out << "}\n";
  return out;
}

void gauss_seidel_block(int N, double* a, int block_size) {
  for (int i = 1; i <= block_size; i++) {
    for (int j = 1; j <= block_size; j++) {
      a[i*N+j] = 0.2 * (a[i*N+j] + a[(i-1)*N+j] + a[(i+1)*N+j] + a[i*N+j-1] + a[i*N+j+1]);
    }
  }
}

void gauss_seidel_sequential(int numiters, int N, int block_size, double* data) {
  for (int iter = 0; iter < numiters; iter++) {
    for (int i = 0; i < N-2; i += block_size) {
      for (int j = 0; j < N-2; j += block_size) {
        gauss_seidel_block(N, &data[N * i + j], block_size);
      }
    }
  }
}
  
template <class node>
class gauss_seidel_sequential_node : public node {
public:
  
  int numiters; int N; int block_size; double* data;

  gauss_seidel_sequential_node(int numiters, int N, int block_size, double* data)
  : numiters(numiters), N(N), block_size(block_size), data(data) { }
  
  void body() {
    gauss_seidel_sequential(numiters, N, block_size, data);
  }
  
};
  
template <class node>
using futures_matrix_type = matrix_type<outset_of<node>*>;

template <class node>
class gauss_seidel_future_body : public node {
public:
  
  enum {
    entry,
    after_force1,
    exit
  };
  
  futures_matrix_type<node>* futures;
  int i, j;
  int N; int block_size; double* data;
  
  gauss_seidel_future_body(futures_matrix_type<node>* futures, int i, int j,
                           int N, int block_size, double* data)
  : futures(futures), i(i), j(j),
  N(N), block_size(block_size), data(data) { }
  
  void body() {
    switch (node::current_block_id) {
      case entry: {
        if (j >= 1) {
          node::force(futures->subscript(i, j - 1),
                      after_force1);
        } else {
          node::jump_to(after_force1);
        }
        break;
      }
      case after_force1: {
        if (i >= 1) {
          node::force(futures->subscript(i - 1, j),
                      exit);
        } else {
          node::jump_to(exit);
        }
        break;
      }
      case exit: {
        int ii = i * block_size;
        int jj = j * block_size;
        gauss_seidel_block(N, &data[N * ii + jj], block_size);
        break;
      }
    }
  }
  
};
  
int pipeline_window_capacity = 4096;
int pipeline_burst_rate = std::max(1, pipeline_window_capacity/8);

void get_pipeline_arguments_from_cmdline() {
  pipeline_window_capacity = pasl::util::cmdline::parse_or_default_int("pipeline_window_capacity",
                                                                       pipeline_window_capacity);
  pipeline_burst_rate = pasl::util::cmdline::parse_or_default_int("pipeline_burst_rate",
                                                                  pipeline_burst_rate);
}

template <class node>
class gauss_seidel_generator : public node {
public:
  
  static constexpr int uninitialized = -1;
  
  enum {
    level_loop_entry,
    level_loop_test,
    diagonal_loop_entry,
    diagonal_loop_body,
    diagonal_loop_test,
    throttle_loop_entry,
    throttle_loop_body,
    throttle_loop_test
  };
  
  futures_matrix_type<node>* futures;
  int N; int block_size; double* data;
  
  int l;
  int c_lo;
  int c_hi;
  int n;
  
  using token_type = struct {
    int l;
    int c_lo;
    int c_hi;
  };
  
  std::deque<token_type> tokens;
  int nb_tokens = 0;
  int nb_tokens_to_pop;
  
  bool need_to_throttle() const {
    return (nb_tokens >= pipeline_window_capacity);
  }
  
  void push_token(int l, int c) {
    token_type t;
    t.l = l;
    t.c_lo = c;
    t.c_hi = c + 1;
    if (! tokens.empty()) {
      token_type s = tokens.back();
      if (s.l == l) {
        tokens.pop_back();
        assert(s.c_hi == c);
        t.c_lo = s.c_lo;
      }
    }
    tokens.push_back(t);
    nb_tokens++;
  }
  
  outset_of<node>* pop_token() {
    assert(! tokens.empty());
    token_type t = tokens.front();
    tokens.pop_front();
    nb_tokens--;
    assert(t.c_hi - t.c_lo > 0);
    int l = t.l;
    int c_lo = t.c_lo++;
    if (t.c_hi - t.c_lo > 0) {
      tokens.push_front(t);
    }
    std::pair<int, int> idx = index_of_cell_at_pos(n, l, c_lo);
    int i = idx.first;
    int j = idx.second;
    return futures->subscript(i, j);
  }
  
  gauss_seidel_generator(futures_matrix_type<node>* futures, int N, int block_size, double* data)
  : futures(futures), N(N), block_size(block_size), data(data),
  l(uninitialized), c_lo(uninitialized), c_hi(uninitialized) { }
  
  gauss_seidel_generator(futures_matrix_type<node>* futures, int N, int block_size, double* data,
                         int l, int c_lo, int c_hi,
                         const std::deque<token_type>& tokens, int nb_tokens)
  : futures(futures), N(N), block_size(block_size), data(data),
  l(l), c_lo(c_lo), c_hi(c_hi), tokens(tokens), nb_tokens(nb_tokens) { }
  
  void body() {
    switch (node::current_block_id) {
      case level_loop_entry: {
        n = (N-2) / block_size;
        if (l == uninitialized) {
          l = 1;
          node::jump_to(level_loop_test);
        } else {
          node::jump_to(diagonal_loop_test);
        }
        break;
      }
      case level_loop_test: {
        if (l <= nb_levels(n)) {
          node::jump_to(diagonal_loop_entry);
        } else {
          // nothing to do
        }
        break;
      }
      case diagonal_loop_entry: {
        c_lo = 0;
        c_hi = nb_cells_in_level(n, l);
        node::jump_to(diagonal_loop_test);
        break;
      }
      case diagonal_loop_body: {
        push_token(l, c_lo);
        std::pair<int, int> idx = index_of_cell_at_pos(n, l, c_lo);
        int i = idx.first;
        int j = idx.second;
        node* f = new gauss_seidel_future_body<node>(futures, i, j, N, block_size, data);
        outset_of<node>* f_out = futures->subscript(i, j);
        c_lo++;
        if (need_to_throttle()) {
          node::future(f, f_out,
                       throttle_loop_entry);
        } else {
          node::future(f, f_out,
                       diagonal_loop_test);
        }
        break;
      }
      case throttle_loop_entry: {
        nb_tokens_to_pop = pipeline_burst_rate;
        node::jump_to(throttle_loop_test);
        break;
      }
      case throttle_loop_body: {
        outset_of<node>* f_out = pop_token();
        nb_tokens_to_pop--;
        node::force(f_out,
                    throttle_loop_test);
        break;
      }
      case throttle_loop_test: {
        if ((tokens.empty()) || (nb_tokens_to_pop == 0)) {
          node::jump_to(diagonal_loop_test);
        } else {
          node::jump_to(throttle_loop_body);
        }
        break;
      }
      case diagonal_loop_test: {
        if (c_lo < c_hi) {
          node::jump_to(diagonal_loop_body);
        } else if (c_hi == nb_cells_in_level(n, l)){
          l++;
          node::jump_to(level_loop_test);
        } else {
          // nothing to do
        }
        break;
      }
    }
  }
  
  size_t size() const {
    return c_hi - c_lo;
  }
  
  pasl::sched::thread_p split() {
    int mid = (c_lo + c_hi) / 2;
    int c_lo2 = mid;
    int c_hi2 = c_hi;
    c_hi = mid;
    auto n = new gauss_seidel_generator(futures, N, block_size, data,
                                        l, c_lo2, c_hi2, tokens, nb_tokens);
    node::split_with(n);
    return n;
  }
  
};
  
template <class node>
class gauss_seidel_parallel : public node {
public:
  
  futures_matrix_type<node>* futures;
  int numiters; int N; int block_size; double* data;
  
  int iter;
  int nb_futures;
  int n;
  
  gauss_seidel_parallel(int numiters, int N, int block_size, double* data)
  : numiters(numiters), N(N), block_size(block_size), data(data) { }
  
  enum {
    entry,
    allocate_futures,
    start_iter,
    end_iter,
    deallocate_futures,
    iter_test
  };

  void body() {
    switch (node::current_block_id) {
      case entry: {
        iter = 0;
        n = (N-2) / block_size;
        futures = new futures_matrix_type<node>(n);
        nb_futures = n * n;
        node::jump_to(allocate_futures);
        break;
      }
      case allocate_futures: {
        node::parallel_for(0, nb_futures, [&] (long i) {
          futures->items[i] = node::allocate_future();
        }, start_iter);
        break;
      }
      case start_iter: {
        node::call(new gauss_seidel_generator<node>(futures, N, block_size, data),
                   end_iter);
        node::listen_on(futures->subscript(n - 1, n - 1));
        break;
      }
      case end_iter: {
        node::force(futures->subscript(n - 1, n - 1),
                    deallocate_futures);
        break;
      }
      case deallocate_futures: {
        node::parallel_for(0, nb_futures, [&] (long i) {
          node::deallocate_future(futures->items[i]);
          futures->items[i] = nullptr;
        }, iter_test);
        iter++;
        break;
      }
      case iter_test: {
        if (iter < numiters) {
          node::jump_to(allocate_futures);
        } else {
          delete futures;
        }
        break;
      }
    }
  }

};
  
void gauss_seidel_by_diagonal(int numiters, int N, int block_size, double* data) {
  assert(((N-2) % block_size) == 0);
  int n = (N-2) / block_size;
  for (int iter = 0; iter < numiters; iter++) {
    for (int l = 1; l <= nb_levels(n); l++) {
      for (int c = 0; c < nb_cells_in_level(n, l); c++) {
        auto idx = index_of_cell_at_pos(n, l, c);
        int i = idx.first * block_size;
        int j = idx.second * block_size;
        gauss_seidel_block(N, &data[N * i + j], block_size);
      }
    }
  }
}

void gauss_seidel_initialize(matrix_type<double>& mtx) {
  int N = mtx.n;
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      mtx.subscript(i, j) = (double) ((i == 25 && j == 25) || (i == N-25 && j == N-25)) ? 500 : 0;
    }
  }
}

double epsilon = 0.001;

int count_nb_diffs(const matrix_type<double>& lhs,
                   const matrix_type<double>& rhs) {
  if (lhs.n != rhs.n) {
    return std::max(lhs.n, rhs.n);
  }
  int nb_diffs = 0;
  int n = lhs.n;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      double diff = std::abs(lhs.subscript(i, j) - rhs.subscript(i, j));
      if (diff > epsilon) {
        nb_diffs++;
      }
    }
  }
  return nb_diffs;
}

} // end namespace

/*---------------------------------------------------------------------*/


/*---------------------------------------------------------------------*/

namespace cmdline = pasl::util::cmdline;

std::deque<pasl::sched::thread_p> todo;

void add_todo(pasl::sched::thread_p t) {
  todo.push_back(t);
}

template <class Body>
class todo_function : public pasl::sched::thread {
public:
  
  Body body;
  
  todo_function(Body body) : body(body) { }
  
  void run() {
    body();
  }
  
  THREAD_COST_UNKNOWN
  
};

void add_todo(std::function<void()> f) {
  add_todo(new todo_function<decltype(f)>(f));
}

void choose_edge_algorithm() {
  cmdline::argmap_dispatch c;
  c.add("simple", [&] {
    direct::edge_algorithm = direct::edge_algorithm_simple;
  });
  c.add("distributed", [&] {
    direct::distributed::default_branching_factor =
    cmdline::parse_or_default_int("branching_factor",
                                  direct::distributed::default_branching_factor);
    direct::dyntree::branching_factor = direct::distributed::default_branching_factor;
    direct::distributed::default_nb_levels =
    cmdline::parse_or_default_int("nb_levels",
                                  direct::distributed::default_nb_levels);
    direct::edge_algorithm = direct::edge_algorithm_distributed;
  });
  c.add("dyntree", [&] {
    direct::edge_algorithm = direct::edge_algorithm_tree;
    direct::dyntree::branching_factor =
    cmdline::parse_or_default_int("branching_factor",
                                  direct::dyntree::branching_factor);
  });
  c.find_by_arg_or_default_key("edge_algo", "tree")();
}

void read_gauss_seidel_params(int& numiters, int& N, int& block_size) {
  numiters = cmdline::parse_or_default_int("numiters", 1);
  N = cmdline::parse_or_default_int("N", 128);
  block_size = cmdline::parse_or_default_int("block_size", 2);
  benchmarks::epsilon = cmdline::parse_or_default_double("epsilon", benchmarks::epsilon);
  assert((N % block_size) == 0);
}

std::string cmd_param = "cmd";

template <class node>
void choose_command() {
  cmdline::argmap_dispatch c;
  c.add("async_microbench", [&] {
    int nb_milliseconds = cmdline::parse_int("nb_milliseconds");
    std::thread timer([&, nb_milliseconds] {
      std::this_thread::sleep_for(std::chrono::milliseconds(nb_milliseconds));
      benchmarks::should_async_microbench_terminate = true;
    });
    timer.detach();
    add_todo(new benchmarks::async_microbench<node>);
  });
  c.add("edge_throughput_microbench", [&] {
    int nb_milliseconds = cmdline::parse_int("nb_milliseconds");
    add_todo(new benchmarks::edge_throughput_microbench<node>(nb_milliseconds));
  });
  c.add("async_bintree", [&] {
    int n = cmdline::parse_or_default_int("n", 1);
    add_todo(new benchmarks::async_bintree<node>(n));
  });
  c.add("future_bintree", [&] {
    int n = cmdline::parse_or_default_int("n", 1);
    add_todo(new benchmarks::future_bintree<node>(n));
  });
  c.add("future_pool", [&] {
    int n = cmdline::parse_or_default_int("n", 1);
    benchmarks::fib_input = cmdline::parse_or_default_int("fib_input", benchmarks::fib_input);
    add_todo(new benchmarks::future_pool<node>(n));
  });
  c.add("parallel_for_test", [&] {
    int n = cmdline::parse_or_default_int("n", 1);
    add_todo(new benchmarks::parallel_for_test<node>(n));
  });
  c.add("seidel_parallel", [&] {
    int numiters;
    int N;
    int block_size;
    bool do_consistency_check = cmdline::parse_or_default_bool("consistency_check", false);
    read_gauss_seidel_params(numiters, N, block_size);
    benchmarks::matrix_type<double>* test_mtx = new benchmarks::matrix_type<double>(N+2, 0.0);
    benchmarks::gauss_seidel_initialize(*test_mtx);
    bool use_reference_solution = cmdline::parse_or_default_bool("reference_solution", false);
    if (use_reference_solution) {
      add_todo([=] {
        benchmarks::gauss_seidel_by_diagonal(numiters, N+2, block_size, &(test_mtx->items[0]));
      });
    } else {
      add_todo(new benchmarks::gauss_seidel_parallel<node>(numiters, N+2, block_size, &(test_mtx->items[0])));
    }
    add_todo([=] {
      if (do_consistency_check) {
        benchmarks::matrix_type<double> reference_mtx(N+2, 0.0);
        benchmarks::gauss_seidel_initialize(reference_mtx);
        benchmarks::gauss_seidel_sequential(numiters, N+2, block_size, &reference_mtx.items[0]);
        int nb_diffs = benchmarks::count_nb_diffs(reference_mtx, *test_mtx);
        assert(nb_diffs == 0);
      }
      delete test_mtx;
    });
  });
  c.add("seidel_sequential", [&] {
    int numiters;
    int N;
    int block_size;
    read_gauss_seidel_params(numiters, N, block_size);
    benchmarks::matrix_type<double>* test_mtx = new benchmarks::matrix_type<double>(N+2, 0.0);
    add_todo(new benchmarks::gauss_seidel_sequential_node<node>(numiters, N+2, block_size, &test_mtx->items[0]));
    add_todo([=] {
      delete test_mtx;
    });
  });
  c.find_by_arg(cmd_param)();
}



void launch() {
  communication_delay = cmdline::parse_or_default_int("communication_delay",
                                                      communication_delay);
  benchmarks::get_pipeline_arguments_from_cmdline();
  cmdline::argmap_dispatch c;
  c.add("direct", [&] {
    choose_edge_algorithm();
    choose_command<direct::node>();
  });
  c.add("portpassing", [&] {
    choose_command<portpassing::node>();
  });
  c.find_by_arg("algo")();
  while (! todo.empty()) {
    pasl::sched::thread_p t = todo.front();
    todo.pop_front();
    pasl::sched::threaddag::launch(t);
  }
}

int main(int argc, char** argv) {
  cmdline::set(argc, argv);
  std::string cmd = pasl::util::cmdline::parse_string(cmd_param);
  if (cmd == "incounter_microbench") {
    benchmarks::launch_incounter_microbenchmark();
  } else {
    pasl::sched::threaddag::init();
    auto start = std::chrono::system_clock::now();
    launch();
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<float> duration = end - start;
    std::cout << "exectime " << duration.count() << std::endl;
    pasl::sched::threaddag::destroy();
  }
  return 0;
}

/***********************************************************************/

