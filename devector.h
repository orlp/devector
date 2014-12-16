/*
    Copyright (c) 2014 Orson Peters

    This software is provided 'as-is', without any express or implied warranty. In no event will the
    authors be held liable for any damages arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose, including commercial
    applications, and to alter it and redistribute it freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not claim that you wrote the
       original software. If you use this software in a product, an acknowledgement in the product
       documentation would be appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be misrepresented as
       being the original software.
    3. This notice may not be removed or altered from any source distribution.
*/


#ifndef DEVECTOR_H
#define DEVECTOR_H

// TODO: Include what you use.
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>



// There is an issue with std::swap. It uses a recursive noexcept declaration for multidimensional
// arrays, but those do not work. This code defines detail::is_nothrow_swappable to solve that.
// http://stackoverflow.com/questions/26793979/why-is-swapping-multidimensional-arrays-not-noexcept
namespace detail {
    namespace swap_adl_tests {
        // If swap ADL finds this then it would call std::swap otherwise (same signature).
        struct tag {};

        template<class T> tag swap(T&, T&);
        template<class T, std::size_t N> tag swap(T (&a)[N], T (&b)[N]);

        // Helper functions to test if an unqualified swap is possible, and if it becomes std::swap.
        template<class, class> std::false_type can_swap(...) noexcept(false);
        template<class T, class U, class = decltype(swap(std::declval<T&>(), std::declval<U&>()))>
        std::true_type can_swap(int) noexcept(
            noexcept(swap(std::declval<T&>(), std::declval<U&>()))
        );

        template<class, class> std::false_type uses_std(...);
        template<class T, class U>
        std::is_same<decltype(swap(std::declval<T&>(), std::declval<U&>())), tag> uses_std(int);

        // Helper templates to determine if swapping is noexcept. The C++11/14 standards have a
        // broken noexcept specification for multidimensional arrays, so we use a template solution.
        template<class T>
        struct is_std_swap_noexcept : std::integral_constant<bool,
            std::is_nothrow_move_constructible<T>::value &&
            std::is_nothrow_move_assignable<T>::value
        > { };

        template<class T, std::size_t N>
        struct is_std_swap_noexcept<T[N]> : is_std_swap_noexcept<T> { };

        template<class T, class U>
        struct is_adl_swap_noexcept : std::integral_constant<bool, noexcept(can_swap<T, U>(0))> { };
    }

    template<class T, class U = T>
    struct is_swappable : std::integral_constant<bool, 
        decltype(detail::swap_adl_tests::can_swap<T, U>(0))::value &&
            (!decltype(detail::swap_adl_tests::uses_std<T, U>(0))::value ||
                (std::is_move_assignable<T>::value && std::is_move_constructible<T>::value))
    > {};

    template<class T, std::size_t N>
    struct is_swappable<T[N], T[N]> : std::integral_constant<bool, 
        decltype(detail::swap_adl_tests::can_swap<T[N], T[N]>(0))::value &&
            (!decltype(detail::swap_adl_tests::uses_std<T[N], T[N]>(0))::value ||
                is_swappable<T, T>::value)
    > {};

    template<class T, class U = T>
    struct is_nothrow_swappable : std::integral_constant<bool, 
        is_swappable<T, U>::value && (
            (decltype(detail::swap_adl_tests::uses_std<T, U>(0))::value &&
                detail::swap_adl_tests::is_std_swap_noexcept<T>::value)
            ||
            (!decltype(detail::swap_adl_tests::uses_std<T, U>(0))::value &&
                detail::swap_adl_tests::is_adl_swap_noexcept<T, U>::value)
        )
    > {};
}


template<class T, class Allocator = std::allocator<T>>
class devector {
private:
    typedef std::allocator_traits<Allocator> alloc_traits;
    typedef devector<T, Allocator> V;

public:
    // Typedefs.
    typedef T                                      value_type;
    typedef Allocator                              allocator_type;
    typedef typename alloc_traits::size_type       size_type;
    typedef typename alloc_traits::difference_type difference_type;
    typedef typename alloc_traits::pointer         pointer;
    typedef typename alloc_traits::const_pointer   const_pointer;
    typedef T&                                     reference;
    typedef const T&                               const_reference;
    typedef pointer                                iterator;
    typedef const_pointer                          const_iterator;
    typedef std::reverse_iterator<iterator>        reverse_iterator;
    typedef std::reverse_iterator<const_iterator>  const_reverse_iterator;

    // Construct/copy/destroy.
    ~devector() noexcept {
        if (!impl.begin_storage) return;
        destruct();
        impl.begin_storage = nullptr;
    }

    devector() noexcept(std::is_nothrow_default_constructible<Allocator>::value) : impl() {
        impl.null();
    }

    explicit devector(const Allocator& alloc) noexcept : impl(alloc) { impl.null(); }

    explicit devector(size_type n, const Allocator& alloc = Allocator()) : impl(alloc) {
        impl.begin_storage = impl.begin_cursor = alloc_traits::allocate(impl, n);
        impl.end_storage = impl.end_cursor = impl.begin_storage + n;
        try { strong_uninitialized_fill(impl.begin_cursor, impl.end_cursor); }
        catch (...) { deallocate(); throw; }
    }

    devector(size_type n, const T& value, const Allocator& alloc = Allocator()) : impl(alloc) {
        impl.begin_storage = impl.begin_cursor = alloc_traits::allocate(impl, n);
        impl.end_storage = impl.end_cursor = impl.begin_storage + n;
        try { strong_uninitialized_fill(impl.begin_cursor, impl.end_cursor, value); }
        catch (...) { deallocate(); throw; }
    }

    template<class InputIterator>
    devector(InputIterator first, InputIterator last, const Allocator& alloc = Allocator())
    : impl(alloc) {
        init_range(first, last, typename std::iterator_traits<InputIterator>::iterator_category());
    }

    devector(const V& other)
    : impl(alloc_traits::select_on_container_copy_construction(other.impl)) {
        init_range(other.begin(), other.end(), std::random_access_iterator_tag());
    }

    devector(const V& other, const Allocator& alloc) : impl(alloc) {
        init_range(other.begin(), other.end(), std::random_access_iterator_tag());
    }

    devector(V&& other) noexcept : impl(std::move(other.impl)) {
        impl.begin_storage = std::move(other.impl.begin_storage);
        impl.end_storage = std::move(other.impl.end_storage);
        impl.begin_cursor = std::move(other.impl.begin_cursor);
        impl.end_cursor = std::move(other.impl.end_cursor);
        other.impl.null();
    }

    devector(V&& other, const Allocator& alloc) : impl(alloc) {
        if (impl.alloc() == other.impl.alloc()) {
            impl.begin_storage = std::move(other.impl.begin_storage);
            impl.end_storage = std::move(other.impl.end_storage);
            impl.begin_cursor = std::move(other.impl.begin_cursor);
            impl.end_cursor = std::move(other.impl.end_cursor);
        } else {
            init_range(std::move_iterator<iterator>(other.begin()),
                       std::move_iterator<iterator>(other.end()),
                       std::random_access_iterator_tag());
            other.destruct();
        }

        other.impl.null();
    }

    devector(std::initializer_list<T> il, const Allocator& alloc = Allocator()) : impl(alloc) {
        init_range(il.begin(), il.end(), std::random_access_iterator_tag());
    }

    V& operator=(const V& other) {
        if (this != &other) {
            copy_assign_propagate_dispatcher(
                other,
                std::integral_constant<bool,
                    alloc_traits::propagate_on_container_copy_assignment::value
                >()
            );
        }

        return *this;
    }

    V& operator=(V&& other)
    noexcept(alloc_traits::propagate_on_container_move_assignment::value) {
        if (this != &other) {
            move_assign_propagate_dispatcher(
                std::move(other),
                std::integral_constant<bool,
                    alloc_traits::propagate_on_container_move_assignment::value
                >()
            );
        }

        return *this;
    }

    V& operator=(std::initializer_list<T> il) { assign(il); return *this; }

    template<class InputIterator>
    typename std::enable_if<
        std::is_base_of<
            std::input_iterator_tag,
            typename std::iterator_traits<InputIterator>::iterator_category
        >::value,
    void>::type assign(InputIterator first, InputIterator last) {
        assign_range(first, last,
                     typename std::iterator_traits<InputIterator>::iterator_category());
    }

    void assign(size_type n, const T& t) {
        reserve(n);
        while (size() > n) pop_back();
        for (iterator it = begin(); it != end; ++it) *it = t;
        while (size() < n) push_back(t);
    }

    void assign(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

    allocator_type get_allocator() const noexcept { return impl; }

    // Iterators.
    iterator               begin()         noexcept { return iterator(impl.begin_cursor); }
    const_iterator         begin()   const noexcept { return iterator(impl.begin_cursor); }
    iterator               end()           noexcept { return iterator(impl.end_cursor); }
    const_iterator         end()     const noexcept { return iterator(impl.end_cursor); }

    reverse_iterator       rbegin()        noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin()  const noexcept { return const_reverse_iterator(end()); }
    reverse_iterator       rend()          noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend()    const noexcept { return const_reverse_iterator(begin()); }

    const_iterator         cbegin()  const noexcept { return begin(); }
    const_iterator         cend()    const noexcept { return end(); }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    const_reverse_iterator crend()   const noexcept { return rend(); }

    // Capacity.
    size_type max_size()       const noexcept { return impl.max_size(); }
    size_type size()           const noexcept { return impl.end_cursor  - impl.begin_cursor; }
    size_type capacity()       const noexcept { return impl.end_storage - impl.begin_storage; }
    size_type capacity_front() const noexcept { return impl.end_cursor  - impl.begin_storage; }
    size_type capacity_back()  const noexcept { return impl.end_storage - impl.begin_cursor; }

    void resize(size_type n) { resize_back(n); }
    void resize(size_type n, const T& t) { resize_back(n, t); }

    void resize_back(size_type n) {
        // FIXME: no effects on exception
        reserve(n);
        while (n < size()) pop_back();
        while (n > size()) emplace_back();
    }

    void resize_back(size_type n, const T& t) {
        // FIXME: no effects on exception
        reserve(n);
        while (n < size()) pop_back();
        while (n > size()) emplace_back(t);
    }
    
    void resize_front(size_type n);
    void resize_front(size_type n, const T& t);

    void reserve(size_type n) { reserve_back(n); }

    void reserve(size_type new_front, size_type new_back) {
        if (new_front > max_size() || new_back > max_size()) {
            throw std::length_error("devector");
        }

        if (capacity_front() >= new_front && capacity_back() >= new_back) return;

        reallocate(new_front - size(), new_back - size());
    }

    void reserve_front(size_type n) {
        if (n > max_size()) throw std::length_error("devector");
        if (capacity_front() >= n) return;

        reallocate(n - size(), impl.end_storage - impl.end_cursor);
    }

    void reserve_back(size_type n) {
        if (n > max_size()) throw std::length_error("devector");
        if (capacity_back() >= n) return;

        reallocate(impl.begin_cursor - impl.begin_storage, n - size());
    }

    void shrink_to_fit() {
        // FIXME: no effects on exception
        size_type n = size();
        if (capacity() > n && n > 0) reallocate(0, 0);
    }

    bool empty() const noexcept { return impl.begin_cursor == impl.end_cursor; }

    // Indexing.
    reference       operator[](size_type i)       noexcept { return impl.begin_cursor[i]; }
    const_reference operator[](size_type i) const noexcept { return impl.begin_cursor[i]; }

    reference at(size_type i) {
        if (i >= size()) throw std::out_of_range("devector");
        return (*this)[i];
    }

    const_reference at(size_type i) const {
        if (i >= size()) throw std::out_of_range("devector");
        return (*this)[i];
    }

    reference         front()       noexcept { return *begin(); }
    const_reference   front() const noexcept { return *begin(); }
    reference         back()        noexcept { return *(end() - 1); }
    const_reference   back()  const noexcept { return *(end() - 1); }
    T*                data()        noexcept { return std::addressof(front()); }
    const T*          data()  const noexcept { return std::addressof(front()); }

    // Modifiers.
    void push_front(const T& x) { emplace_front(x); }
    void push_front(T&& x)      { emplace_front(std::move(x)); }
    void push_back(const T& x)  { emplace_back(x); }
    void push_back(T&& x)       { emplace_back(std::move(x)); }

    void pop_front() { alloc_traits::destroy(impl, std::addressof(*impl.begin_cursor++)); }
    void pop_back()  { alloc_traits::destroy(impl, std::addressof(*--impl.end_cursor)); }

    template<class... Args>
    void emplace_front(Args&&... args) {
        if (impl.begin_cursor == impl.begin_storage) req_storage_front();
        alloc_traits::construct(impl, std::addressof(*(begin() - 1)),
                                std::forward<Args>(args)...);
        --impl.begin_cursor; // We do this after constructing for strong exception safety.
    }

    template<class... Args>
    void emplace_back(Args&&... args) {
        if (impl.end_cursor == impl.end_storage) req_storage_back();
        alloc_traits::construct(impl, std::addressof(*end()),
                                std::forward<Args>(args)...);
        ++impl.end_cursor; // We do this after constructing for strong exception safety.
    }

    // CORRECT MARKER

    // There is no standards-compliant way of implementing an efficient emplace and insert
    template<class... Args>
    iterator emplace(const_iterator position, Args&&... args) {
        // TODO: move to unitialized memory broken
        // TODO: strong exception guarantee
        difference_type dist_front = position - begin();
        difference_type dist_back = end() - position;

        if (dist_front < dist_back) {
            // TODO: do not move data twice
            if (impl.begin_cursor == impl.begin_storage) req_storage_front();

            if (dist_front == 0) {
                alloc_traits::construct(impl, std::addressof(*(begin() - 1)),
                                        std::forward<Args>(args)...);
            } else {
                T tmp(std::forward<Args>(args)...);

                try {
                    alloc_traits::construct(impl, std::addressof(*(begin() - 1)),
                                            std::move_if_noexcept(front()));
                } catch (...) {
                    alloc_traits::destroy(impl, std::addressof(*(begin() - 1)));
                    throw;
                }


            }

            --impl.begin_cursor;
        } else {
        }

        // const_iterator to iterator
        return begin() + dist_front;
    }

    iterator insert(const_iterator position, const T& t) { return emplace(position, t); }
    iterator insert(const_iterator position, T&& t) { return emplace(position, std::move(t)); }
    iterator insert(const_iterator position, size_type n, const T& t);

    iterator insert(const_iterator position, std::initializer_list<T> il) {
        return insert(position, il.begin(), il.end());
    }
    
    template<class InputIterator>
    typename std::enable_if<
        std::is_base_of<
            std::input_iterator_tag,
            typename std::iterator_traits<InputIterator>::iterator_category
        >::value,
    iterator>::type insert(const_iterator position, InputIterator first, InputIterator last);

    iterator erase(const_iterator position) { return erase(position, position + 1); }

    iterator erase(const_iterator first, const_iterator last) {
        difference_type n = last - first;
        difference_type retpos = first - begin();

        if (first - begin() < end() - last) {
            std::move_backward(begin(), first, first + n);
            while (n--) alloc_traits::destroy(impl, std::addressof(*impl.begin_cursor++));
        } else {
            std::move(last, end(), last - n);
            while (n--) alloc_traits::destroy(impl, std::addressof(*--impl.end_cursor));
        }

        return begin() + retpos;
    }

    void swap(V& other)
    noexcept(!alloc_traits::propagate_on_container_swap::value ||
             detail::is_nothrow_swappable<Allocator>::value) {
        using std::swap;

        if (alloc_traits::propagate_on_container_swap::value) {
            swap(impl.alloc(), other.impl.alloc());
        }

        swap(impl.begin_storage, other.impl.begin_storage);
        swap(impl.end_storage, other.impl.end_storage);
        swap(impl.begin_cursor, other.impl.begin_cursor);
        swap(impl.end_cursor, other.impl.end_cursor);
    }

    void clear() noexcept {
        while (begin() != end()) {
            try { pop_back(); } catch (...) { }
        }
    }

private:
    // Empty base class optimization.
    struct impl_ : public Allocator {
        impl_() noexcept(std::is_nothrow_default_constructible<Allocator>::value)
        : Allocator() { }

        impl_(const Allocator& alloc) noexcept
        : Allocator(alloc) { }

        impl_(Allocator&& alloc) noexcept
        : Allocator(std::move(alloc)) { }

        void null() {
            begin_storage = end_storage = nullptr;
            begin_cursor = end_cursor = nullptr;
        }

        Allocator& alloc() { return *this; }
        const Allocator& alloc() const { return *this; }
        
        pointer begin_storage; // storage[0]
        pointer end_storage; // storage[n] (one-past-end)
        pointer begin_cursor; // devector[0]
        pointer end_cursor; // devector[n] (one-past-end)
    } impl;

    // Deallocates the stored memory. Does not leave the devector in a valid state!
    void deallocate() noexcept {
        alloc_traits::deallocate(impl, impl.begin_storage, capacity());
    }

    // Deletes all elements and deallocates memory. Does not leave the devector in a valid state,
    // unless an exception was thrown in an element destructor. If that's the case the 
    void destruct() {
        clear();
        deallocate();
    }

    // Reallocate with space_front free space in the front, and space_back in the back.
    void reallocate(size_type space_front, size_type space_back) {
        size_type alloc_size = space_front + size() + space_back;

        pointer new_storage = alloc_traits::allocate(impl, alloc_size);
        pointer new_begin_cursor = new_storage + space_front;

        try { strong_uninitialized_move(begin(), end(), new_begin_cursor); }
        catch (...) { alloc_traits::deallocate(impl, new_storage, alloc_size); throw; }

        destruct();
        impl.begin_storage = new_storage;
        impl.end_storage = new_storage + alloc_size;
        impl.begin_cursor = new_begin_cursor;
        impl.end_cursor = impl.end_storage - space_back;
    }

    // Fills first, last with constructed elements with args. Strong exception guarantee, cleans up
    // if an exception occurs.
    template<class... Args>
    pointer strong_uninitialized_fill(pointer first, pointer last, Args&&... args) {
        pointer current = first;

        try {
            while (current != last) {
                alloc_traits::construct(impl, std::addressof(*current++), args...);
            }
        } catch (...) {
            while (first != current) alloc_traits::destroy(impl, std::addressof(*first++));
            throw;
        }

        return current;
    }

    // Copies from the range [first, last) into the uninitialized range starting at d_first. Strong
    // exception guarantee, cleans up if an exception occurs.
    template<class InputIterator>
    pointer strong_uninitialized_copy(InputIterator first, InputIterator last, pointer d_first) {
        pointer current = d_first;

        try {
            while (first != last) {
                alloc_traits::construct(impl, std::addressof(*current++), *first++);
            }
        } catch (...) {
            while (d_first != current) alloc_traits::destroy(impl, std::addressof(*d_first++));
            throw;
        }

        return current;
    }

    // Moves from the range [first, last) into the uninitialized range starting at d_first. Strong
    // exception guarantee, cleans up if an exception occurs.
    template<class InputIterator>
    pointer strong_uninitialized_move(InputIterator first, InputIterator last, pointer d_first) {
        pointer current = d_first;

        try {
            while (first != last) {
                alloc_traits::construct(impl,
                                        std::addressof(*current++),
                                        std::move_if_noexcept(*first++));
            }
        } catch (...) {
            while (d_first != current) alloc_traits::destroy(impl, std::addressof(*d_first++));
            throw;
        }

        return current;
    }


    // Initializes the devector with copies from [first, last). Strong exception guarantee.
    template<class InputIterator>
    void init_range(InputIterator first, InputIterator last, std::random_access_iterator_tag) {
        size_type n = last - first;

        if (n > 0) {
            impl.begin_storage = impl.begin_cursor = alloc_traits::allocate(impl, n);
            impl.end_storage = impl.end_cursor = impl.begin_storage + n;
            strong_uninitialized_copy(first, last, impl.begin_cursor);
        }
    }

    // Initializes the devector with copies from [first, last). Strong exception guarantee.
    template<class InputIterator>
    void init_range(InputIterator first, InputIterator last, std::bidirectional_iterator_tag) {
        impl.null();
        while (first != last) push_back(*first++);
    }


    template<class InputIterator>
    void assign_range(InputIterator first, InputIterator last, std::random_access_iterator_tag) {
        size_type n = last - first;
        reserve(n);

        while (size() > n) pop_back();
        for (auto& el : *this) el = *first++;
        while (first != last) push_back(*first++);
    }

    template<class InputIterator>
    void assign_range(InputIterator first, InputIterator last, std::bidirectional_iterator_tag) {
        auto it = begin();
        while (it != end() && first != last) *it++ = *first++;
        while (it != end()) pop_back();
        while (first != last) push_back(*first++);
    }

    // Helper functions for move assignment. Second argument is 
    // alloc_traits::propagate_on_container_move_assignment::value.
    void move_assign_propagate_dispatcher(V&& other, std::true_type) noexcept {
        destruct();
        impl.alloc() = std::move(other.impl.alloc());
        impl.begin_storage = std::move(other.impl.begin_storage);
        impl.end_storage = std::move(other.impl.end_storage);
        impl.begin_cursor = std::move(other.impl.begin_cursor);
        impl.end_cursor = std::move(other.impl.end_cursor);
        other.impl.null();
    }

    void move_assign_propagate_dispatcher(V&& other, std::false_type) {
        if (impl.alloc() != other.impl.alloc()) {
            destruct();
            impl.null();
        }

        assign(std::move_iterator<iterator>(other.begin()),
               std::move_iterator<iterator>(other.end()));

        other.destruct();
        other.impl.null();
    }
    
    // Helper functions for copy assignment. Second argument is 
    // alloc_traits::propagate_on_container_copy_assignment::value.
    void copy_assign_propagate_dispatcher(const V& other, std::true_type) {
        if (impl.alloc() != other.impl.alloc()) {
            destruct();
            impl.null();
        }

        impl.alloc() = other.impl.alloc();
        assign(other.begin(), other.end());
    }

    void copy_assign_propagate_dispatcher(const V& other, std::false_type) {
        assign(other.begin(), other.end());
    }
};


// Comparison operators.
template<class T, class Allocator>
bool operator==(const devector<T, Allocator>& lhs, const devector<T, Allocator>& rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

template<class T, class Allocator>
bool operator< (const devector<T, Allocator>& lhs, const devector<T, Allocator>& rhs) {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

template<class T, class Allocator>
bool operator!=(const devector<T, Allocator>& lhs, const devector<T, Allocator>& rhs) {
    return !(lhs == rhs);
}

template<class T, class Allocator>
bool operator> (const devector<T, Allocator>& lhs, const devector<T, Allocator>& rhs) {
    return rhs < lhs;
}

template<class T, class Allocator>
bool operator<=(const devector<T, Allocator>& lhs, const devector<T, Allocator>& rhs) {
    return !(rhs < lhs);
}

template<class T, class Allocator>
bool operator>=(const devector<T, Allocator>& lhs, const devector<T, Allocator>& rhs) {
    return !(lhs < rhs);
}

template<class T, class Allocator>
void swap(devector<T, Allocator>& lhs, devector<T, Allocator>& rhs)
noexcept(noexcept(lhs.swap(rhs))) {
    lhs.swap(rhs);
}

/*
    Implementation notes.

    May throw exceptions:
        Allocator default construction.
        All value_type constructors.
        Allocation.

    May not throw exceptions mandated by the standard:
        Allocator copy/move construction.
        All alloc_traits::(const_)pointer operations.
        Deallocation.
        
    May throw exceptions but makes effects undefined:
        value_type destructor.

    Valid moved-from state: impl.null().
*/

#endif
