//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//
//	@filename:
//		CColRefSet.h
//
//	@doc:
//		Implementation of column reference sets based on bitset
//---------------------------------------------------------------------------
#ifndef GPOS_CColRefSet_H
#define GPOS_CColRefSet_H

#include "gpos/base.h"
#include "gpos/common/CBitSet.h"

#include "gpopt/base/CColRef.h"


#define GPOPT_COLREFSET_SIZE 1024

namespace gpopt
{
// fwd decl
class CColRefSet;

// short hand for colref set array
using CColRefSetArray = CDynamicPtrArray<CColRefSet, CleanupRelease>;

// hash map mapping CColRef -> CColRefSet
using ColRefToColRefSetMap =
	CHashMap<CColRef, CColRefSet, CColRef::HashValue, CColRef::Equals,
			 CleanupNULL<CColRef>, CleanupRelease<CColRefSet>>;

// hash map mapping INT -> CColRef
using IntToColRefMap =
	CHashMap<INT, CColRef, gpos::HashValue<INT>, gpos::Equals<INT>,
			 CleanupDelete<INT>, CleanupNULL<CColRef>>;

//---------------------------------------------------------------------------
//	@class:
//		CColRefSet
//
//	@doc:
//		Column reference sets based on bitsets
//
//		Redefine accessors by bit index to be private to make super class'
//		member functions inaccessible
//
//---------------------------------------------------------------------------
class CColRefSet : public CBitSet
{
	// bitset iter needs to access internals
	friend class CColRefSetIter;

private:
	// determine if bit is set
	BOOL Get(ULONG ulBit) const;

	// set given bit; return previous value
	BOOL ExchangeSet(ULONG ulBit);

	// clear given bit; return previous value
	BOOL ExchangeClear(ULONG ulBit);

public:
	// ctor
	explicit CColRefSet(CMemoryPool *mp,
						ULONG ulSizeBits = GPOPT_COLREFSET_SIZE);

	explicit CColRefSet(CMemoryPool *mp, const CColRefSet &);

	// ctor, copy from col refs array
	CColRefSet(CMemoryPool *mp, const CColRefArray *colref_array,
			   ULONG ulSizeBits = GPOPT_COLREFSET_SIZE);

	// dtor
	~CColRefSet() override;

	// determine if bit is set
	BOOL FMember(const CColRef *colref) const;

	// return random member
	CColRef *PcrAny() const;

	// return first member
	CColRef *PcrFirst() const;

	// include column
	void Include(const CColRef *colref);

	// include column array
	void Include(const CColRefArray *colref_array);

	// include column set
	void Include(const CColRefSet *pcrs);

	// remove column
	void Exclude(const CColRef *colref);

	// remove column array
	void Exclude(const CColRefArray *colref_array);

	// remove column set
	void Exclude(const CColRefSet *pcrs);

	// replace column with another column
	void Replace(const CColRef *pcrOut, const CColRef *pcrIn);

	// replace column array with another column array
	void Replace(const CColRefArray *pdrgpcrOut, const CColRefArray *pdrgpcrIn);

	// check if the current colrefset is a subset of any of the colrefsets
	// in the given array
	BOOL FContained(const CColRefSetArray *pdrgpcrs);

	// check if current colrefset intersects with the given colrefset
	BOOL FIntersects(const CColRefSet *pcrs);

	// convert to array
	CColRefArray *Pdrgpcr(CMemoryPool *mp) const;

	// convert to id colref map
	IntToColRefMap *Phmicr(CMemoryPool *mp) const;

	// hash function
	ULONG HashValue();

	// debug print
	IOstream &OsPrint(IOstream &os) const override;
	IOstream &OsPrint(IOstream &os, ULONG ulLenMax) const;

	// extract all column ids
	void ExtractColIds(CMemoryPool *mp, ULongPtrArray *colids) const;

	// extract the index(relative position) in current CColRefSet
	// notice that: Whenever the current CColRefSet changes, the index will also change.
	void ExtractIndex(const CColRef *querycr, ULONG *colidx) const;

	// are the columns in the column reference set covered by the array of column ref sets
	static BOOL FCovered(CColRefSetArray *pdrgpcrs, CColRefSet *pcrs);

};	// class CColRefSet


// shorthand for printing
inline IOstream &
operator<<(IOstream &os, CColRefSet &crf)
{
	return crf.OsPrint(os);
}

}  // namespace gpopt

#endif	// !GPOS_CColRefSet_H


// EOF
