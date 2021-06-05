//*****************************************************************
//
//	$file: typedefs.h $
//	$author: Martin Fouilleul $
//	$date: 23/36/2015 $
//	$revision: $
//	$note: (C) 2015 by Martin Fouilleul - all rights reserved $
//
//*****************************************************************
#ifndef __TYPEDEFS_H_
#define __TYPEDEFS_H_

#include<inttypes.h>
#include<float.h>	//FLT_MAX/MIN etc...

#ifndef __cplusplus
#include<stdbool.h>
#endif //__cplusplus

	typedef int8_t	int8;
	typedef int16_t	int16;
	typedef int32_t	int32;
	typedef int64_t	int64;

	typedef uint8_t	byte;
	typedef uint8_t	 uint8;
	typedef uint16_t uint16;
	typedef uint32_t uint32;
	typedef uint64_t uint64;

	typedef float	float32;
	typedef double	float64;

	//NOTE(martin): short typedefs versions

	typedef int8_t	i8;
	typedef int16_t	i16;
	typedef int32_t	i32;
	typedef int64_t	i64;

	typedef uint8_t	 u8;
	typedef uint16_t u16;
	typedef uint32_t u32;
	typedef uint64_t u64;

	typedef float	f32;
	typedef double	f64;

#endif //__TYPEDEFS_H_
