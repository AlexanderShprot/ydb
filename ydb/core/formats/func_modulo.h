#pragma once 
#include "func_common.h" 
#include "clickhouse_type_traits.h" 
 
namespace NKikimr::NArrow {
 
struct TModulo { 
 
    static constexpr const char * Name = "mod"; 
 
    template <typename TRes, typename TArg0, typename TArg1> 
    static constexpr EnableIfInteger<TRes> Call(arrow::compute::KernelContext*, TArg0 lhs, TArg1 rhs, arrow::Status* st) { 
        static_assert(std::is_same<TRes, TArg0>::value && std::is_same<TRes, TArg1>::value, ""); 
        if (ARROW_PREDICT_FALSE(rhs == 0)) { 
            *st = arrow::Status::Invalid("divide by zero"); 
            return 0; 
        } 
        return static_cast<TRes>(lhs) % static_cast<TRes>(rhs); 
    } 
 
    template <typename TRes, typename TArg0, typename TArg1> 
    static constexpr EnableIfFloatingPoint<TRes> Call(arrow::compute::KernelContext*, TArg0 lhs, TArg1 rhs, arrow::Status* st) { 
        static_assert(std::is_same<TRes, TArg0>::value && std::is_same<TRes, TArg1>::value, ""); 
        if (static_cast<typename TToInteger<TArg1>::Type::c_type>(rhs) == 0) { 
            *st = arrow::Status::Invalid("divide by zero"); 
            return 0; 
        } 
        return static_cast<typename TToInteger<TArg0>::Type::c_type>(lhs) % static_cast<typename TToInteger<TArg1>::Type::c_type>(rhs); 
    } 
}; 
 
} 
