// cxx_function.hpp: major evolution for std::function
// Copyright 2015 by David Krauss.
// This source is released under the MIT license, http://opensource.org/licenses/MIT

#ifndef INCLUDED_CXX_FUNCTION_HPP
#define INCLUDED_CXX_FUNCTION_HPP

#include <exception>
#include <functional> // for std::bad_function_call and std::mem_fn
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cxx_function {

template< typename ... sig >
class function;

template< typename ... sig >
class unique_function;

template< typename alloc, typename ... sig >
class function_container;

template< typename alloc, typename ... sig >
class unique_function_container;

template< typename >
struct any_piecewise_construct_tag {};

namespace impl {

/* Implement a vtable using metaprogramming. Why?
    1. Implement without polymorphic template instantiations (would need 2N of them).
    2. Eliminate overhead and ABI issues associated with RTTI and weak linkage.
    3. Allow static data entries as well as functions.
*/
enum class dispatch_slot {
    destructor,
    move_constructor,
    copy_constructor,
    target_access,
    target_type,
    
    base_index
};
constexpr int operator + ( dispatch_slot e ) { return static_cast< int >( e ); }

// "Abstract" base class for the island inside the wrapper class, e.g. std::function.
// This must appear first in the derived class layout.
template< typename ... sig >
struct erasure_base {
    typedef std::tuple<
        void (erasure_base::*)(), // destructor
        void (erasure_base::*)( void * ) &&, // move constructor
        void (erasure_base::*)( void * ) const &, // copy constructor
        
        void const * (erasure_base::*)() const, // target access
        std::type_info const &, // target_type
        
        sig erasure_base::* ... // dispatchers
    > dispatch_table;
    
    dispatch_table const & table;
    
    constexpr erasure_base( dispatch_table const & in_table )
        : table( in_table ) {}
};

// Generic "virtual" functions to manage the wrapper payload lifetime.
template< typename derived >
struct erasure_special {
    void destroy()
        { static_cast< derived * >( this )-> ~ derived(); }
    void move( void * dest ) &&
        { new (dest) derived( std::move( * static_cast< derived * >( this ) ) ); }
    void copy( void * dest ) const &
        { new (dest) derived( * static_cast< derived const * >( this ) ); }
};

// This accessor avoids instantiating erasure_special::copy for non-copyable types.
template< typename derived >
constexpr typename std::enable_if< std::is_copy_constructible< derived >::value >::type
( derived::* ( * erasure_copy ( derived const & ) ) ) ( void * ) const &
    { return & derived::erasure_special::copy; }
template< typename derived >
constexpr typename std::enable_if< ! std::is_copy_constructible< derived >::value >::type
( derived::* ( * erasure_copy ( derived const & ) ) ) ( void * ) const &
    { return nullptr; }

template< typename >
struct const_unsafe_case;

#define DISPATCH_BASE_CASE( NAME ) \
template< typename derived, std::size_t n, typename ... sig > \
struct NAME ## _dispatch { \
    static_assert ( sizeof ... (sig) == 0, "An unsupported function signature was detected." ); \
    void operator () () = delete; \
};

#define UNPACK(...) __VA_ARGS__
#define IGNORE(...)

#define DISPATCH_CASE( QUALS, UNSAFE, NAME, IMPL ) \
template< typename derived, std::size_t table_index, typename ret, typename ... args, typename ... sig > \
struct NAME ## _dispatch< derived, table_index, ret( args ... ) QUALS, sig ... > \
    : NAME ## _dispatch< derived, table_index+1, sig ... \
        UNSAFE (, const_unsafe_case< ret( args ... ) >) > { \
    using NAME ## _dispatch< derived, table_index+1, sig ... \
        UNSAFE (, const_unsafe_case< ret( args ... ) >) >::operator (); \
    ret operator () ( args ... a ) QUALS { UNPACK IMPL } \
};

#define DISPATCH_CQ( MACRO, UNSAFE, QUALS ) MACRO( QUALS, UNSAFE ) MACRO( const QUALS, IGNORE )
#define DISPATCH_CV( MACRO, UNSAFE, QUALS ) DISPATCH_CQ( MACRO, UNSAFE, QUALS ) DISPATCH_CQ( MACRO, IGNORE, volatile QUALS )
#define DISPATCH_CVREFQ( MACRO, QUALS ) DISPATCH_CV( MACRO, IGNORE, & QUALS ) DISPATCH_CV( MACRO, IGNORE, && QUALS )
#define DISPATCH_ALL( MACRO ) DISPATCH_CV( MACRO, UNPACK, ) DISPATCH_CVREFQ( MACRO, )

// A bogus base member is undefined, [expr.static.cast]/12.
// However, [expr.mptr.oper]/4 mentions dynamic type, which suggests that the overall usage is OK.
template< typename base, typename derived, typename t, typename actual_base >
t base::* ptm_cast( t actual_base::* ptm )
    { return static_cast< t base::* >( static_cast< t derived::* >( ptm ) ); }

#define DISPATCH_TABLE( NAME, TARGET_TYPE, TPARAM, TARG ) \
template< UNPACK TPARAM > \
typename erasure_base< sig ... >::dispatch_table const NAME< UNPACK TARG >::table = { \
    ptm_cast< erasure_base< sig ... >, NAME >( & NAME::destroy ), \
    ptm_cast< erasure_base< sig ... >, NAME >( & NAME::move ), \
    ptm_cast< erasure_base< sig ... >, NAME >( & NAME::copy ), \
    ptm_cast< erasure_base< sig ... >, NAME >( & NAME::target_access ), \
    typeid (TARGET_TYPE), \
    ptm_cast< erasure_base< sig ... >, NAME >( static_cast< sig NAME::* >( & NAME::operator () ) ) ... \
};

DISPATCH_BASE_CASE( null )
#define NULL_CASE( QUALS, UNSAFE ) DISPATCH_CASE( QUALS, IGNORE, null, ( throw std::bad_function_call{}; ) )
DISPATCH_ALL( NULL_CASE )
#undef NULL_CASE

template< typename ... sig >
struct null_erasure
    : erasure_base< sig ... >
    , erasure_special< null_erasure< sig ... > >
    , null_dispatch< null_erasure< sig ... >, 0, sig ... > {
    static const typename erasure_base< sig ... >::dispatch_table table;
    
    void const * target_access() const { return nullptr; }
    
    null_erasure() noexcept
        : null_erasure::erasure_base( table ) {}
};

DISPATCH_TABLE( null_erasure, void, ( typename ... sig ), ( sig ... ) )


template< typename t >
struct add_reference
    { typedef t & type; };
template< typename t >
struct add_reference< t && >
    { typedef t && type; };

DISPATCH_BASE_CASE( local )
#define LOCAL_CASE( QUALS, UNSAFE ) DISPATCH_CASE( QUALS, IGNORE, local, ( \
    return static_cast< typename add_reference< derived QUALS >::type >( * this ) \
        .target( std::forward< args >( a ) ... ); \
) )
DISPATCH_ALL( LOCAL_CASE )
#undef LOCAL_CASE

template< typename target_type, typename ... sig >
struct local_erasure
    : erasure_base< sig ... >
    , erasure_special< local_erasure< target_type, sig ... > >
    , local_dispatch< local_erasure< target_type, sig ... >, 0, sig ... > {
    static const typename erasure_base< sig ... >::dispatch_table table;
    
    target_type target;
    
    void const * target_access() const { return & target; }
    
    template< typename ... arg >
    local_erasure( arg && ... a )
        : local_erasure::erasure_base( table )
        , target( std::forward< arg >( a ) ... ) {}
};

DISPATCH_TABLE( local_erasure, target_type, ( typename target_type, typename ... sig ), ( target_type, sig ... ) )

DISPATCH_BASE_CASE( ptm )
#define PTM_CASE( QUALS, UNSAFE ) DISPATCH_CASE( QUALS, IGNORE, ptm, ( \
    return std::mem_fn( static_cast< derived const volatile & >( * this ).target )( std::forward< args >( a ) ... ); \
) )
DISPATCH_ALL( PTM_CASE )
#undef PTM_CASE

template< typename target_type, typename ... sig >
struct ptm_erasure
    : erasure_base< sig ... >
    , erasure_special< ptm_erasure< target_type, sig ... > >
    , ptm_dispatch< ptm_erasure< target_type, sig ... >, 0, sig ... > {
    static const typename erasure_base< sig ... >::dispatch_table table;
    
    target_type target;
    
    void const * target_access() const { return & target; }
    
    ptm_erasure( target_type a )
        : ptm_erasure::erasure_base( table )
        , target( a ) {}
};

DISPATCH_TABLE( ptm_erasure, target_type, ( typename target_type, typename ... sig ), ( target_type, sig ... ) )


DISPATCH_BASE_CASE( allocator )
#define ALLOCATOR_CASE( QUALS, UNSAFE ) DISPATCH_CASE( QUALS, IGNORE, allocator, ( \
    return ( * static_cast< typename add_reference< derived QUALS >::type >( * this ).target ) \
        ( std::forward< args >( a ) ... ); \
) )
DISPATCH_ALL( ALLOCATOR_CASE )
#undef ALLOCATOR_CASE

template< typename allocator, typename target_type, typename ... sig >
struct allocator_erasure
    : erasure_base< sig ... >
    , allocator
    , erasure_special< allocator_erasure< allocator, target_type, sig ... > >
    , allocator_dispatch< allocator_erasure< allocator, target_type, sig ... >, 0, sig ... > {
    using allocator_erasure::erasure_special::destroy;
    static const typename erasure_base< sig ... >::dispatch_table table;
    
    typename std::allocator_traits< allocator >::pointer target;
    
    allocator & alloc() { return static_cast< allocator & >( * this ); }
    target_type * target_address() { return std::addressof( * target ); }
    void const * target_access() const { return std::addressof( * target ); }
    
    template< typename ... arg >
    allocator_erasure( allocator in_alloc, arg && ... a )
        : allocator_erasure::erasure_base( table )
        , allocator( in_alloc )
        , target( std::allocator_traits< allocator >::allocate( alloc(), 1 ) )
        { std::allocator_traits< allocator >::construct( alloc(), target_address(), std::forward< arg >( a ) ... ); }
    
    ~ allocator_erasure()
        { std::allocator_traits< allocator >::destroy( alloc(), target_address() ); }
    
    allocator_erasure( allocator_erasure && o ) noexcept
        : allocator_erasure::erasure_base( table )
        , allocator( std::move( o ) )
        , target( std::move( o.target ) )
        { o.target = nullptr; }
    
    allocator_erasure( allocator_erasure const & o )
        : allocator_erasure::erasure_base( table )
        , allocator( o )
        , target( std::allocator_traits< allocator >::allocate( alloc(), 1 ) )
        { std::allocator_traits< allocator >::construct( alloc(), target_address(), * o.target ); }
};

DISPATCH_TABLE( allocator_erasure, target_type, ( typename allocator, typename target_type, typename ... sig ), ( allocator, target_type, sig ... ) )


DISPATCH_BASE_CASE( wrapper )
#define WRAPPER_CASE( QUALS, UNSAFE ) DISPATCH_CASE( QUALS, UNSAFE, wrapper, ( \
    auto && self = static_cast< typename add_reference< derived QUALS >::type >( * this ); \
    return ( std::forward< decltype (self) >( self ).erasure() \
            .* std::get< + dispatch_slot::base_index + table_index >( self.erasure().table ) ) \
        ( std::forward< args >( a ) ... ); \
) )
DISPATCH_ALL( WRAPPER_CASE )
#undef WRAPPER_CASE

template< typename derived, std::size_t n, typename ret, typename ... args, typename ... more >
struct wrapper_dispatch< derived, n, const_unsafe_case< ret( args ... ) >, more ... >
    : wrapper_dispatch< derived, n, more ... > {
    using wrapper_dispatch< derived, n, more ... >::operator ();
    [[deprecated( "It is unsafe to call a std::function of non-const signature through a const access path." )]]
    ret operator () ( args ... a ) const {
        return const_cast< derived & >( static_cast< derived const & >( * this ) )
            ( std::forward< args >( a ) ... );
    }
};

template< typename ... sig >
class wrapper_base
    : public wrapper_dispatch< wrapper_base< sig ... >, 0, sig ... > {
    typedef erasure_base< sig ... > erasure_xface;
    
    std::aligned_storage< sizeof (void *[4]) >::type storage;
    void * storage_address() { return & storage; }
    
    // These functions enter or recover from invalid states.
    // They get on the right side of [basic.life]/7.4, but mind the exceptions.
    
    void destroy() noexcept
        { ( erasure() .* std::get< + dispatch_slot::destructor >( erasure().table ) )(); }
    
    void init( std::nullptr_t = {} ) noexcept
        { new (storage_address()) null_erasure< sig ... >; }
    
    void init( wrapper_base && o ) noexcept {
        ( std::move( o ).erasure() .* std::get< + dispatch_slot::move_constructor >( o.erasure().table ) )( storage_address() );
        o = nullptr;
    }
    
    void init( wrapper_base const & o )
        { ( o.erasure() .* std::get< + dispatch_slot::copy_constructor >( o.erasure().table ) )( storage_address() ); }
    
    void init( unique_function< sig ... > const & o ) = delete;
    
    // Queries on potential targets.
    template< typename, typename = void >
    struct is_compatibly_wrapped : std::false_type {};
    template< typename source >
    struct is_compatibly_wrapped< source, typename std::enable_if<
            std::is_same< wrapper_base, typename std::decay< source >::type::wrapper_base >::value >::type >
        : std::true_type {};
    
    template< typename, typename = void >
    struct is_small : std::false_type {};
    template< typename source >
    struct is_small< source, typename std::enable_if<
            sizeof (local_erasure< source, sig ... >) <= sizeof (storage)
            && alignof (source) <= alignof (decltype (storage))
            && std::is_nothrow_move_constructible< source >::value >::type >
        : std::true_type {};
    
    // Local erasures.
    template< typename source, typename ... arg >
    typename std::enable_if< ! is_compatibly_wrapped< source >::value && is_small< source >::value >::type
    init( any_piecewise_construct_tag< source >, arg && ... a )
        { new (storage_address()) local_erasure< source, sig ... >( std::forward< arg >( a ) ... ); }
    
    template< typename allocator, typename source, typename ... arg >
    typename std::enable_if< ! is_compatibly_wrapped< source >::value && is_small< source >::value >::type
    init( std::allocator_arg_t, allocator const &, any_piecewise_construct_tag< source > t, arg && ... a )
        { init( t, std::forward< arg >( a ) ... ); }
    
    // Pointers are local callables.
    template< typename t >
    void init( any_piecewise_construct_tag< t * >, t * p ) {
        if ( p ) new (storage_address()) local_erasure< t *, sig ... >( p );
        else init();
    }
    // PTMs are like local callables.
    template< typename t, typename c >
    void init( any_piecewise_construct_tag< t c::* >, t c::* ptm ) {
        if ( ptm ) new (storage_address()) ptm_erasure< t c::*, sig ... >( ptm );
        else init();
    }
    // Allocated erasures.
    template< typename allocator, typename source, typename ... arg >
    typename std::enable_if< ! is_compatibly_wrapped< source >::value && ! is_small< source >::value >::type
    init( std::allocator_arg_t, allocator const & alloc, any_piecewise_construct_tag< source >, arg && ... a ) {
        typedef allocator_erasure< allocator, source, sig ... > erasure;
        // TODO: Add a new erasure template to rebind the allocator and put the fancy pointer on the heap.
        static_assert ( sizeof (erasure) <= sizeof storage, "Fancy pointer is too big for polymorphic function wrapper." );
        new (storage_address()) erasure( alloc, std::forward< arg >( a ) ... );
    }
    template< typename source, typename ... arg >
    typename std::enable_if< ! is_compatibly_wrapped< source >::value && ! is_small< source >::value >::type
    init( any_piecewise_construct_tag< source > t, arg && ... a )
        { init( std::allocator_arg, std::allocator< source >{}, t, std::forward< arg >( a ) ... ); }
    
    // Adoption by copy/move is implemented in terms of in-place construction.
    template< typename source >
    typename std::enable_if< ! is_compatibly_wrapped< source >::value >::type
    init( source && s )
        { init( any_piecewise_construct_tag< typename std::decay< source >::type >{}, std::forward< source >( s ) ); }
    
    template< typename allocator, typename source >
    typename std::enable_if< ! is_compatibly_wrapped< source >::value >::type
    init( std::allocator_arg_t at, allocator const & alloc, source && s )
        { init( at, alloc, any_piecewise_construct_tag< typename std::decay< source >::type >{}, std::forward< source >( s ) ); }
    
public:
    #define ERASURE_ACCESS( QUALS, UNSAFE ) \
        erasure_xface QUALS erasure() QUALS { return reinterpret_cast< erasure_xface QUALS >( storage ); }
    DISPATCH_CVREFQ( ERASURE_ACCESS, )
    #undef ERASURE_ACCESS
    
    template< typename ... arg >
    wrapper_base( arg && ... a )
        noexcept(noexcept( init( std::forward< arg >( a ) ... ) ))
        { init( std::forward< arg >( a ) ... ); }
    
    ~ wrapper_base() noexcept
        { destroy(); }
    
    template< typename source >
    wrapper_base & operator = ( source && s )
        noexcept( is_compatibly_wrapped< source >::value || is_small< source >::value )
    try {
        destroy();
        init( std::forward< source >( s ) );
        return * this;
    } catch (...) {
        init();
        throw;
    }
    
    void swap( wrapper_base & other )
        { std::swap( * this, other ); }
    
    std::type_info const & target_type() const noexcept
        { return std::get< + dispatch_slot::target_type >( erasure().table ); }
    
    template< typename want >
    want const * target() const noexcept {
        if ( typeid (want) != target_type() ) return nullptr;
        return static_cast< want const * >( ( erasure() .* std::get< + dispatch_slot::target_access >( erasure().table ) )() );
    }
    template< typename want >
    want * target() noexcept
        { return const_cast< want * >( static_cast< wrapper_base const & >( * this ).target< want >() ); }
    
    explicit operator bool () const noexcept
        { return target_type() != typeid (void); }
};

}

template< typename ... sig >
struct function
    : private impl::wrapper_base< sig ... > {
    friend class impl::wrapper_base< sig ... >;
    
    using function::wrapper_base::wrapper_base;
    using function::wrapper_base::operator ();
    using function::wrapper_base::operator =;
    using function::wrapper_base::swap;
    using function::wrapper_base::target;
    using function::wrapper_base::target_type;
    using function::wrapper_base::operator bool;
};

template< typename ... sig >
bool operator == ( function< sig ... > const & a, std::nullptr_t )
    { return !a; }
template< typename ... sig >
bool operator != ( function< sig ... > const & a, std::nullptr_t )
    { return a; }
template< typename ... sig >
bool operator == ( std::nullptr_t, function< sig ... > const & a )
    { return !a; }
template< typename ... sig >
bool operator != ( std::nullptr_t, function< sig ... > const & a )
    { return a; }

}

#endif
