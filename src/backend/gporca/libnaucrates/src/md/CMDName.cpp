//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CMDName.cpp
//
//	@doc:
//		Metadata name of objects
//---------------------------------------------------------------------------

#include "naucrates/md/CMDName.h"

#include "gpos/base.h"
#include "gpos/string/CWStringDynamic.h"

using namespace gpmd;


//---------------------------------------------------------------------------
//	@function:
//		CMDName::CMDName
//
//	@doc:
//		Constructor
//		Creates a deep copy of the provided string
//
//---------------------------------------------------------------------------
CMDName::CMDName(CMemoryPool *mp, const CWStringBase *str)
	: m_name(nullptr), m_deep_copy(true)
{
	m_name = GPOS_NEW(mp) CWStringConst(mp, str->GetBuffer());
}

//---------------------------------------------------------------------------
//	@function:
//		CMDName::CMDName
//
//	@doc:
//		ctor
//		Depending on the value of the the owns_memory argument, the string object
//		can become property of the CMDName object
//
//---------------------------------------------------------------------------
CMDName::CMDName(const CWStringConst *str, BOOL owns_memory)
	: m_name(str), m_deep_copy(owns_memory)
{
	GPOS_ASSERT(nullptr != m_name);
	GPOS_ASSERT(m_name->IsValid());
}

//---------------------------------------------------------------------------
//	@function:
//		CMDName::CMDName
//
//	@doc:
//		Shallow copy constructor
//
//---------------------------------------------------------------------------
CMDName::CMDName(const CMDName &name)
	: m_name(name.GetMDName()), m_deep_copy(false)
{
	GPOS_ASSERT(nullptr != m_name->GetBuffer());
	GPOS_ASSERT(m_name->IsValid());
}


//---------------------------------------------------------------------------
//	@function:
//		CMDName::~CMDName
//
//	@doc:
//		dtor
//
//---------------------------------------------------------------------------
CMDName::~CMDName()
{
	// Some of `m_name` is reference only, there is no guarantee
	// that the referenced object lives longer than the CMDName
	// object when running the destructor of a process.

	if (m_deep_copy)
	{
		GPOS_DELETE(m_name);
	}
}

// EOF
