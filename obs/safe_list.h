// Observable Library
// Copyright (c) 2016 David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef OBS_SAFE_LIST_H_INCLUDED
#define OBS_SAFE_LIST_H_INCLUDED
#pragma once

#include <cassert>
#include <chrono>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace obs {

// A STL-like list which is safe to remove/add items from multiple
// threads while it's being iterated by multiple threads too.
template<typename T>
class safe_list {
public:
  class iterator;

private:
  // A node in the linked list.
  struct node {
    // Pointer to a slot or an observer instance.
    //
    // As we cannot modify the list when we are in for-loops iterating
    // the list, we can temporally mark nodes as disabled changing
    // this "value" member to nullptr when we use erase(), Then when
    // the list is not iterated anymore (m_ref==0), we call
    // delete_nodes() to remove all disabled nodes from the list.
    //
    // We try to skip nodes with value=nullptr in the iteration
    // process (iterator::operator++). But it isn't possible to ensure
    // that an iterator will always return a non-nullptr value (so the
    // client have to check the return value from iterators).
    T* value;

    // Number of locks for this node, it means the number of iterators
    // being used and currently pointing to this node. (I.e. number of
    // times an iterator::operator*() was called for this node and the
    // iterator is still alive.)
    //
    // This variable is incremented/decremented only when
    // m_mutex_nodes is locked.
    int locks;

    // Next node in the list. It's nullptr for the last node in the list.
    node* next;

    // Thread used to add the node to the list (i.e. the thread where
    // safe_list::push_back() was used). We suppose that the same
    // thread will remove the node.
    std::thread::id creator_thread;

    // Pointer to the first iterator that locked this node in the same
    // thread it was created. It is used to unlock() the node when
    // erase() is called in the same iterator loop/call.
    iterator* creator_thread_iterator;

    node(T* value = nullptr)
      : value(value),
        locks(0),
        next(nullptr),
        creator_thread(std::this_thread::get_id()),
        creator_thread_iterator(nullptr) {
    }

    node(const node&) = delete;
    node& operator=(const node&) = delete;

    // Returns true if we are using this node from the same thread
    // where it was created. (i.e. the thread where
    // safe_list::push_back() was used.)
    //
    // This function is used to know if an iterator that locks/unlocks
    // a node belongs to the same "creator thread," so when we erase()
    // the node, we can (must) unlock all those iterators.
    bool in_creator_thread() const {
      return (creator_thread == std::this_thread::get_id());
    }

    // Locks the node by the given iterator. It means that
    // iterator::operator*() is going to return the node's value so we
    // can use it. (E.g. in case of a slot, we can call the slot
    // function.)
    void lock(iterator* it);

    // Indicates that the node is not being used by the given iterator
    // anymore. So we could delete it in case that erase() is
    // called.
    void unlock(iterator* it);

    // Notify to all iterators in the "creator thread" that they don't
    // own a node lock anymore. It's used to erase() the node.
    void unlock_all();
  };

  // Mutex used to modify the linked-list (m_first/m_last and node::next).
  std::mutex m_mutex_nodes;

  // Used to iterate the list from the first element to the last one.
  node* m_first = nullptr;

  // Used to add new items at the end of the list (with push_back()).
  node* m_last = nullptr;

  // "m_ref" indicates the number of times this list is being iterated
  // simultaneously.  When "m_ref" reaches 0, the delete_nodes()
  // function is called to delete all unused nodes (unlocked nodes
  // with value=nullptr). While "m_ref" > 0 it means that we shouldn't
  // remove nodes (so we can ensure that an actual node reference is
  // still valid until the next unref()).
  std::mutex m_mutex_ref;
  int m_ref = 0;

  // Flag that indicates if some node was erased and delete_nodes()
  // should iterate the whole list to clean disabled nodes (nodes with
  // value = nullptr).
  bool m_delete_nodes = false;

public:

  // A STL-like iterator for safe_list. It is not a fully working
  // iterator, and shouldn't be used directly, it's expected to be
  // used only in range-based for loops.
  //
  // The iterator works in the following way:
  //
  // 1. It adds a new reference (ref()/unref()) to the safe_list so
  //    nodes are not deleted while the iterator is alive
  // 2. operator*() locks the node and returns its value, when a node
  //    is locked we can use it's "value" (call a slot/observer)
  // 3. When the iterator is incremented (operator++) it unlocks the
  //    previous node and goes to the next one (the next node is not
  //    locked until we use operator*() again)
  class iterator : public std::iterator<std::forward_iterator_tag, T*> {
  public:
    friend struct node;

    iterator(safe_list& list, node* node)
      : m_list(list),
        m_node(node),
        m_locked(false),
        m_next_iterator(nullptr) {
      m_list.ref();
    }

    // Cannot copy iterators
    iterator(const iterator&) = delete;
    iterator& operator=(const iterator&) = delete;

    // We can only move iterators
    iterator(iterator&& other)
      : m_list(other.m_list),
        m_node(other.m_node),
        m_locked(false),
        m_next_iterator(nullptr) {
      assert(!other.m_locked);
      m_list.ref();
    }

    ~iterator() {
      assert(!m_locked);
      m_list.unref();
    }

    // Called when erase() is used from the iterators created in the
    // "creator thread".
    void notify_unlock(const node* node) {
      if (m_locked) {
        assert(m_node == node);
        assert(m_locked);
        m_locked = false;
      }
    }

    // Unlocks the current m_node and goes to the next enabled
    // (node::value != nullptr) node. It doesn't lock the new found
    // node, operator*() is the member function that locks the node.
    iterator& operator++() {
      std::lock_guard<std::mutex> lock(m_list.m_mutex_nodes);
      assert(m_node);
      if (m_node) {
        if (m_locked) {
          m_node->unlock(this);
          m_locked = false;
        }
        m_node = m_node->next;
      }
      return *this;
    }

    // Tries to lock the node and returns it's value. If the node was
    // already deleted, it will return nullptr and the client will
    // need to call operator++() again. We cannot guarantee that this
    // function will return a value != nullptr.
    T* operator*() {
      std::lock_guard<std::mutex> lock(m_list.m_mutex_nodes);
      assert(m_node);
      if (m_node->value) {
        // Add a lock to m_node before we access to its value. It's
        // used to keep track of how many iterators are using the node
        // in the list.
        if (!m_locked) {
          m_node->lock(this);
          m_locked = true;
        }

        assert(m_node->value);
        return m_node->value;
      }
      else {
        // We might try to iterate to the following nodes to get a
        // m_node->value != nullptr, but we might reach the last node
        // and should return nullptr anyway (also the next
        // operator++() call would be an invalid "++it" call using the
        // end of the list).
        return nullptr;
      }
    }

    // This can be used only to compare an iterator created from
    // begin() (in "this" pointer) with end() ("other" argument).
    bool operator!=(const iterator& other) const {
      if (m_node && other.m_node)
        return (m_node != other.m_node->next);
      else
        return false;
    }

  private:
    safe_list& m_list;

    // Current node being iterated. It is never nullptr.
    node* m_node;

    // True if this iterator has added a lock to the "m_node"
    bool m_locked;

    // Next iterator locking the same "m_node" from its creator
    // thread.
    iterator* m_next_iterator;
  };

  safe_list() {
  }

  ~safe_list() {
    assert(m_ref == 0);
#if _DEBUG
    {
      std::lock_guard<std::mutex> lock(m_mutex_nodes);
      for (node* node=m_first; node; node=node->next) {
        assert(!node->locks);
      }
    }
#endif
    delete_nodes(true);

    assert(m_first == m_last);
    assert(m_first == nullptr);
  }

  void push_back(T* value) {
    node* n = new node(value);

    std::lock_guard<std::mutex> lock(m_mutex_nodes);
    if (!m_first)
      m_first = m_last = n;
    else {
      m_last->next = n;
      m_last = n;
    }
  }

  void erase(T* value) {
    // We add a ref to avoid calling delete_nodes().
    ref();
    m_mutex_nodes.lock();

    for (node* node=m_first; node; node=node->next) {
      if (node->value == value) {
        // We disable the node so it isn't used anymore by other
        // iterators.
        assert(node->value);
        node->unlock_all();
        node->value = nullptr;
        m_delete_nodes = true;

        // In this case we should wait until the node is unlocked,
        // because after erase() the client could be deleting the
        // value that we are using in other thread.
        if (node->locks) {
          m_mutex_nodes.unlock();

          // Wait until the node is completely unlocked by other
          // threads.
          while (node->locks)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

          // Lock again the list.
          m_mutex_nodes.lock();
        }

        // The node will be finally deleted when we leave the
        // iteration loop (m_ref==0, i.e. the end() iterator is
        // destroyed)
        break;
      }
    }

    m_mutex_nodes.unlock();
    unref();
  }

  iterator begin() {
    std::lock_guard<std::mutex> lock(m_mutex_nodes);
    return iterator(*this, m_first);
  }

  iterator end() {
    return iterator(*this, m_last);
  }

  void ref() {
    std::lock_guard<std::mutex> lock(m_mutex_ref);
    ++m_ref;
    assert(m_ref > 0);
  }

  void unref() {
    std::lock_guard<std::mutex> lock(m_mutex_ref);
    assert(m_ref > 0);
    --m_ref;
    if (m_ref == 0 && m_delete_nodes) {
      delete_nodes(false);
      m_delete_nodes = false;
    }
  }

private:
  // Deletes nodes from the list. If "all" is true, deletes all nodes,
  // if it's false, it deletes only nodes with value == nullptr, which
  // are nodes that were disabled
  void delete_nodes(bool all) {
    node* prev = nullptr;
    node* next = nullptr;

    for (node* node=m_first; node; node=next) {
      next = node->next;

      if ((all || !node->value) && !node->locks) {
        if (prev) {
          prev->next = next;
          if (node == m_last)
            m_last = prev;
        }
        else {
          m_first = next;
          if (node == m_last)
            m_last = m_first;
        }

        delete node;
      }
      else
        prev = node;
    }
  }

};

template<typename T>
void safe_list<T>::node::lock(iterator* it) {
  ++locks;
  assert(locks > 0);

  // If we are in the creator thread, we add this iterator in the
  // "creator thread iterators" linked-list so the iterator is
  // notified in case that the node is erased.
  if (in_creator_thread()) {
    it->m_next_iterator = creator_thread_iterator;
    creator_thread_iterator = it;
  }
}

template<typename T>
void safe_list<T>::node::unlock(iterator* it) {
  assert(it);

  // In this case we are unlocking just one iterator, if we are in the
  // creator thread, we've to remove this iterator from the "creator
  // thread iterators" linked-list.
  if (in_creator_thread()) {
    iterator* prev = nullptr;
    iterator* next = nullptr;
    for (auto it2=creator_thread_iterator; it2; it2=next) {
      next = it2->m_next_iterator;
      if (it2 == it) {
        if (prev)
          prev->m_next_iterator = next;
        else
          creator_thread_iterator = next;

        break;
      }
      prev = it2;
    }
  }

  assert(locks > 0);
  --locks;
}

// In this case we've called erase() to delete this node, so we have
// to unlock the node from the creator thread if we are in the
// creator thread.
template<typename T>
void safe_list<T>::node::unlock_all() {
  if (in_creator_thread()) {
    // Notify to all iterators in the creator thread that they don't
    // have the node locked anymore. In this way we can continue the
    // erase() call.
    for (auto it=creator_thread_iterator; it; it=it->m_next_iterator) {
      it->notify_unlock(this);

      assert(locks > 0);
      --locks;
    }
    creator_thread_iterator = nullptr;
  }
}

} // namespace obs

#endif
