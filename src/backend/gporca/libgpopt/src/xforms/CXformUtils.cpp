//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright 2012 EMC Corp.
//
//	@filename:
//		CXformUtils.cpp
//
//	@doc:
//		Implementation of xform utilities
//---------------------------------------------------------------------------


#include "gpopt/xforms/CXformUtils.h"

#include "gpos/base.h"
#include "gpos/error/CMessage.h"
#include "gpos/error/CMessageRepository.h"
#include "gpos/memory/CAutoMemoryPool.h"

#include "gpopt/base/CConstraintConjunction.h"
#include "gpopt/base/CConstraintNegation.h"
#include "gpopt/base/CKeyCollection.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/exception.h"
#include "gpopt/hints/CHintUtils.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CLogicalAssert.h"
#include "gpopt/operators/CLogicalBitmapTableGet.h"
#include "gpopt/operators/CLogicalCTEConsumer.h"
#include "gpopt/operators/CLogicalCTEProducer.h"
#include "gpopt/operators/CLogicalDynamicBitmapTableGet.h"
#include "gpopt/operators/CLogicalDynamicGet.h"
#include "gpopt/operators/CLogicalDynamicIndexOnlyGet.h"
#include "gpopt/operators/CLogicalGbAgg.h"
#include "gpopt/operators/CLogicalGbAggDeduplicate.h"
#include "gpopt/operators/CLogicalGet.h"
#include "gpopt/operators/CLogicalInnerJoin.h"
#include "gpopt/operators/CLogicalNAryJoin.h"
#include "gpopt/operators/CLogicalSelect.h"
#include "gpopt/operators/CLogicalSequenceProject.h"
#include "gpopt/operators/CPhysicalInnerHashJoin.h"
#include "gpopt/operators/CScalarAssertConstraint.h"
#include "gpopt/operators/CScalarAssertConstraintList.h"
#include "gpopt/operators/CScalarBitmapBoolOp.h"
#include "gpopt/operators/CScalarBitmapIndexProbe.h"
#include "gpopt/operators/CScalarCmp.h"
#include "gpopt/operators/CScalarDMLAction.h"
#include "gpopt/operators/CScalarIdent.h"
#include "gpopt/operators/CScalarIf.h"
#include "gpopt/operators/CScalarProjectElement.h"
#include "gpopt/operators/CScalarProjectList.h"
#include "gpopt/operators/CScalarSubquery.h"
#include "gpopt/operators/CScalarSubqueryAll.h"
#include "gpopt/operators/CScalarSubqueryQuantified.h"
#include "gpopt/operators/CScalarValuesList.h"
#include "gpopt/operators/CScalarWindowFunc.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "gpopt/search/CGroupExpression.h"
#include "gpopt/search/CGroupProxy.h"
#include "gpopt/xforms/CDecorrelator.h"
#include "gpopt/xforms/CSubqueryHandler.h"
#include "gpopt/xforms/CXformExploration.h"
#include "naucrates/base/CDatumInt8GPDB.h"
#include "naucrates/md/CMDIdGPDB.h"
#include "naucrates/md/IMDCheckConstraint.h"
#include "naucrates/md/IMDScalarOp.h"
#include "naucrates/md/IMDTypeBool.h"
#include "naucrates/md/IMDTypeInt4.h"
#include "naucrates/md/IMDTypeInt8.h"
#include "naucrates/md/IMDTypeOid.h"
#include "naucrates/statistics/CFilterStatsProcessor.h"

using namespace gpopt;

// predicates less selective than this threshold
// (selectivity is greater than this number) lead to
// disqualification of a btree index on an AO table
#define AO_TABLE_BTREE_INDEX_SELECTIVITY_THRESHOLD 0.10

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::ExfpLogicalJoin2PhysicalJoin
//
//	@doc:
//		Check the applicability of logical join to physical join xform
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformUtils::ExfpLogicalJoin2PhysicalJoin(CExpressionHandle &exprhdl)
{
	// if scalar predicate has a subquery, we must have an
	// equivalent logical Apply expression created during exploration;
	// no need for generating a physical join
	if (exprhdl.DeriveHasSubquery(2))
	{
		return CXform::ExfpNone;
	}

	return CXform::ExfpHigh;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::ExfpSemiJoin2CrossProduct
//
//	@doc:
//		Check the applicability of semi join to cross product xform
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformUtils::ExfpSemiJoin2CrossProduct(CExpressionHandle &exprhdl)
{
#ifdef GPOS_DEBUG
	COperator::EOperatorId op_id = exprhdl.Pop()->Eopid();
#endif	// GPOS_DEBUG
	GPOS_ASSERT(COperator::EopLogicalLeftSemiJoin == op_id ||
				COperator::EopLogicalLeftAntiSemiJoin == op_id ||
				COperator::EopLogicalLeftAntiSemiJoinNotIn == op_id);

	CColRefSet *pcrsUsed = exprhdl.DeriveUsedColumns(2);
	CColRefSet *pcrsOuterOutput =
		exprhdl.DeriveOutputColumns(0 /*child_index*/);
	if (0 == pcrsUsed->Size() || !pcrsOuterOutput->ContainsAll(pcrsUsed))
	{
		// xform is inapplicable of join predicate uses columns from join's inner child
		return CXform::ExfpNone;
	}

	return CXform::ExfpHigh;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::ExfpExpandJoinOrder
//
//	@doc:
//		Check the applicability of N-ary join expansion
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformUtils::ExfpExpandJoinOrder(CExpressionHandle &exprhdl,
								 const CXform *xform)
{
	// With optimizer_join_order set to 'query' or 'exhaustive', the
	// 'query' join order will expand the join even if it contains
	// outer refs, using another method to get the promise.
	// Therefore we also allow expansion for 'exhaustive2'
	// when we have outer refs.
	if (exprhdl.DeriveHasSubquery(exprhdl.Arity() - 1) ||
		(exprhdl.HasOuterRefs() &&
		 CXform::ExfExpandNAryJoinDPv2 != xform->Exfid()))
	{
		// subqueries must be unnested before applying xform
		return CXform::ExfpNone;
	}

	if (nullptr != exprhdl.Pgexpr())
	{
		// if handle is attached to a group expression, transformation is applied
		// to the Memo and we need to check if stats are derivable on child groups
		CGroup *pgroup = exprhdl.Pgexpr()->Pgroup();
		CAutoMemoryPool amp;
		CMemoryPool *mp = amp.Pmp();
		if (!pgroup->FStatsDerivable(mp))
		{
			// stats must be derivable before applying xforms
			return CXform::ExfpNone;
		}

		const ULONG arity = exprhdl.Arity();
		for (ULONG ul = 0; ul < arity; ul++)
		{
			CGroup *pgroupChild = (*exprhdl.Pgexpr())[ul];
			if (!pgroupChild->FScalar() && !pgroupChild->FStatsDerivable(mp))
			{
				// stats must be derivable on every child
				return CXform::ExfpNone;
			}
		}
	}

	return CXform::ExfpHigh;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FInlinableCTE
//
//	@doc:
//		Check whether a CTE should be inlined
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FInlinableCTE(ULONG ulCTEId)
{
	CCTEInfo *pcteinfo = COptCtxt::PoctxtFromTLS()->Pcteinfo();
	CExpression *pexprProducer = pcteinfo->PexprCTEProducer(ulCTEId);
	GPOS_ASSERT(nullptr != pexprProducer);
	CFunctionProp *pfp = pexprProducer->DeriveFunctionProperties();

	CPartInfo *ppartinfoCTEProducer = pexprProducer->DerivePartitionInfo();
	GPOS_ASSERT(nullptr != ppartinfoCTEProducer);

	return IMDFunction::EfsVolatile > pfp->Efs() &&
		   !pfp->NeedsSingletonExecution() &&
		   (0 == ppartinfoCTEProducer->UlConsumers() ||
			1 == pcteinfo->UlConsumers(ulCTEId));
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PcrsFKey
//
//	@doc:
//		Helper to construct a foreign key by collecting columns that appear
//		in equality predicates with primary key columns;
//		return NULL if no foreign key could be constructed
//
//---------------------------------------------------------------------------
CColRefSet *
CXformUtils::PcrsFKey(
	CMemoryPool *mp,
	CExpressionArray *pdrgpexpr,  // array of scalar conjuncts
	CColRefSet *prcsOutput,		  // output columns of outer expression
	CColRefSet *pcrsKey			  // a primary key of a inner expression
)
{
	GPOS_ASSERT(nullptr != pdrgpexpr);
	GPOS_ASSERT(nullptr != pcrsKey);
	GPOS_ASSERT(nullptr != prcsOutput);

	// collected columns that are part of primary key and used in equality predicates
	CColRefSet *pcrsKeyParts = GPOS_NEW(mp) CColRefSet(mp);

	// FK columns
	CColRefSet *pcrsFKey = GPOS_NEW(mp) CColRefSet(mp);
	const ULONG ulConjuncts = pdrgpexpr->Size();
	for (ULONG ul = 0; ul < ulConjuncts; ul++)
	{
		CExpression *pexprConjunct = (*pdrgpexpr)[ul];
		if (!CPredicateUtils::FPlainEquality(pexprConjunct))
		{
			continue;
		}

		CColRef *pcrFst = const_cast<CColRef *>(
			CScalarIdent::PopConvert((*pexprConjunct)[0]->Pop())->Pcr());
		CColRef *pcrSnd = const_cast<CColRef *>(
			CScalarIdent::PopConvert((*pexprConjunct)[1]->Pop())->Pcr());
		if (pcrsKey->FMember(pcrFst) && prcsOutput->FMember(pcrSnd))
		{
			pcrsKeyParts->Include(pcrFst);
			pcrsFKey->Include(pcrSnd);
		}
		else if (pcrsKey->FMember(pcrSnd) && prcsOutput->FMember(pcrFst))
		{
			pcrsFKey->Include(pcrFst);
			pcrsKeyParts->Include(pcrSnd);
		}
	}

	// check if collected key parts constitute a primary key
	if (!pcrsKeyParts->Equals(pcrsKey))
	{
		// did not succeeded in building foreign key
		pcrsFKey->Release();
		pcrsFKey = nullptr;
	}
	pcrsKeyParts->Release();

	return pcrsFKey;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PcrsFKey
//
//	@doc:
//		Return a foreign key pointing from outer expression to inner
//		expression;
//		return NULL if no foreign key could be extracted
//
//---------------------------------------------------------------------------
CColRefSet *
CXformUtils::PcrsFKey(CMemoryPool *mp, CExpression *pexprOuter,
					  CExpression *pexprInner, CExpression *pexprScalar)
{
	// get inner expression key
	CKeyCollection *pkc = pexprInner->DeriveKeyCollection();
	if (nullptr == pkc)
	{
		// inner expression has no key
		return nullptr;
	}
	// get outer expression output columns
	CColRefSet *prcsOutput = pexprOuter->DeriveOutputColumns();

	CExpressionArray *pdrgpexpr =
		CPredicateUtils::PdrgpexprConjuncts(mp, pexprScalar);
	CColRefSet *pcrsFKey = nullptr;

	const ULONG ulKeys = pkc->Keys();
	for (ULONG ulKey = 0; ulKey < ulKeys; ulKey++)
	{
		CColRefArray *pdrgpcrKey = pkc->PdrgpcrKey(mp, ulKey);

		CColRefSet *pcrsKey = GPOS_NEW(mp) CColRefSet(mp);
		pcrsKey->Include(pdrgpcrKey);
		pdrgpcrKey->Release();

		// attempt to construct a foreign key based on current primary key
		pcrsFKey = PcrsFKey(mp, pdrgpexpr, prcsOutput, pcrsKey);
		pcrsKey->Release();

		if (nullptr != pcrsFKey)
		{
			// succeeded in finding FK
			break;
		}
	}

	pdrgpexpr->Release();

	return pcrsFKey;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprRedundantSelectForDynamicIndex
//
//	@doc:
// 		Add a redundant SELECT node on top of Dynamic (Bitmap) IndexGet to
//		to be able to use index predicate in partition elimination
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprRedundantSelectForDynamicIndex(
	CMemoryPool *mp,
	CExpression *
		pexpr  // input expression is a dynamic (bitmap) IndexGet with an optional Select on top
)
{
	GPOS_ASSERT(nullptr != pexpr);

	COperator::EOperatorId op_id = pexpr->Pop()->Eopid();
	GPOS_ASSERT(COperator::EopLogicalDynamicIndexGet == op_id ||
				COperator::EopLogicalDynamicIndexOnlyGet == op_id ||
				COperator::EopLogicalDynamicBitmapTableGet == op_id ||
				COperator::EopLogicalSelect == op_id);

	CExpression *pexprRedundantScalar = nullptr;
	if (COperator::EopLogicalDynamicIndexGet == op_id ||
		COperator::EopLogicalDynamicIndexOnlyGet == op_id ||
		COperator::EopLogicalDynamicBitmapTableGet == op_id)
	{
		// no residual predicate, use index lookup predicate only
		pexpr->AddRef();
		// reuse index lookup predicate
		(*pexpr)[0]->AddRef();
		pexprRedundantScalar = (*pexpr)[0];

		return GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CLogicalSelect(mp),
										pexpr, pexprRedundantScalar);
	}

	// there is a residual predicate in a SELECT node on top of DynamicIndexGet,
	// we create a conjunction of both residual predicate and index lookup predicate
	CExpression *pexprChild = (*pexpr)[0];
#ifdef GPOS_DEBUG
	COperator::EOperatorId eopidChild = pexprChild->Pop()->Eopid();
	GPOS_ASSERT(COperator::EopLogicalDynamicIndexGet == eopidChild ||
				COperator::EopLogicalDynamicIndexOnlyGet == eopidChild ||
				COperator::EopLogicalDynamicBitmapTableGet == eopidChild);
#endif	// GPOS_DEBUG

	CExpression *pexprIndexLookupPred = (*pexprChild)[0];
	CExpression *pexprResidualPred = (*pexpr)[1];
	pexprRedundantScalar = CPredicateUtils::PexprConjunction(
		mp, pexprIndexLookupPred, pexprResidualPred);

	pexprChild->AddRef();

	return GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CLogicalSelect(mp),
									pexprChild, pexprRedundantScalar);
}


#ifdef GPOS_DEBUG
//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FSwapableJoinType
//
//	@doc:
//		Check whether the given join type is swapable
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FSwapableJoinType(COperator::EOperatorId op_id)
{
	return (COperator::EopLogicalLeftSemiJoin == op_id ||
			COperator::EopLogicalLeftAntiSemiJoin == op_id ||
			COperator::EopLogicalLeftAntiSemiJoinNotIn == op_id ||
			COperator::EopLogicalInnerJoin == op_id);
}
#endif	// GPOS_DEBUG

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprSwapJoins
//
//	@doc:
//		Compute a swap of the two given joins;
//		the function returns null if swapping cannot be done
//
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprSwapJoins(CMemoryPool *mp, CExpression *pexprTopJoin,
							CExpression *pexprBottomJoin)
{
#ifdef GPOS_DEBUG
	COperator::EOperatorId eopidTop = pexprTopJoin->Pop()->Eopid();
	COperator::EOperatorId eopidBottom = pexprBottomJoin->Pop()->Eopid();
#endif	// GPOS_DEBUG

	GPOS_ASSERT(FSwapableJoinType(eopidTop) && FSwapableJoinType(eopidBottom));
	GPOS_ASSERT_IMP(
		COperator::EopLogicalInnerJoin == eopidTop,
		COperator::EopLogicalLeftSemiJoin == eopidBottom ||
			COperator::EopLogicalLeftAntiSemiJoin == eopidBottom ||
			COperator::EopLogicalLeftAntiSemiJoinNotIn == eopidBottom);

	// get used columns by the join predicate of top join
	CColRefSet *pcrsUsed = (*pexprTopJoin)[2]->DeriveUsedColumns();

	// get output columns of bottom join's children
	const CColRefSet *pcrsBottomOuter =
		(*pexprBottomJoin)[0]->DeriveOutputColumns();
	const CColRefSet *pcrsBottomInner =
		(*pexprBottomJoin)[1]->DeriveOutputColumns();

	BOOL fDisjointWithBottomOuter = pcrsUsed->IsDisjoint(pcrsBottomOuter);
	BOOL fDisjointWithBottomInner = pcrsUsed->IsDisjoint(pcrsBottomInner);
	if (!fDisjointWithBottomOuter && !fDisjointWithBottomInner)
	{
		// top join uses columns from both children of bottom join;
		// join swap is not possible
		return nullptr;
	}

	CExpression *pexprChild = (*pexprBottomJoin)[0];
	CExpression *pexprChildOther = (*pexprBottomJoin)[1];
	if (fDisjointWithBottomOuter && !fDisjointWithBottomInner)
	{
		pexprChild = (*pexprBottomJoin)[1];
		pexprChildOther = (*pexprBottomJoin)[0];
	}

	CExpression *pexprRight = (*pexprTopJoin)[1];
	CExpression *pexprScalar = (*pexprTopJoin)[2];
	COperator *pop = pexprTopJoin->Pop();
	pop->AddRef();
	pexprChild->AddRef();
	pexprRight->AddRef();
	pexprScalar->AddRef();
	CExpression *pexprNewBottomJoin =
		GPOS_NEW(mp) CExpression(mp, pop, pexprChild, pexprRight, pexprScalar);

	pexprScalar = (*pexprBottomJoin)[2];
	pop = pexprBottomJoin->Pop();
	pop->AddRef();
	pexprChildOther->AddRef();
	pexprScalar->AddRef();

	// return a new expression with the two joins swapped
	return GPOS_NEW(mp)
		CExpression(mp, pop, pexprNewBottomJoin, pexprChildOther, pexprScalar);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprPushGbBelowJoin
//
//	@doc:
//		Push a Gb, optionally with a having clause below the child join;
//		if push down fails, the function returns NULL expression
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprPushGbBelowJoin(CMemoryPool *mp, CExpression *pexpr)
{
	COperator::EOperatorId op_id = pexpr->Pop()->Eopid();

	GPOS_ASSERT(COperator::EopLogicalGbAgg == op_id ||
				COperator::EopLogicalGbAggDeduplicate == op_id ||
				COperator::EopLogicalSelect == op_id);

	CExpression *pexprSelect = nullptr;
	CExpression *pexprGb = pexpr;
	if (COperator::EopLogicalSelect == op_id)
	{
		pexprSelect = pexpr;
		pexprGb = (*pexpr)[0];
	}

	CExpression *pexprJoin = (*pexprGb)[0];
	CExpression *pexprPrjList = (*pexprGb)[1];
	CExpression *pexprOuter = (*pexprJoin)[0];
	CExpression *pexprInner = (*pexprJoin)[1];
	CExpression *pexprScalar = (*pexprJoin)[2];
	CLogicalGbAgg *popGbAgg = CLogicalGbAgg::PopConvert(pexprGb->Pop());

	CColRefSet *pcrsOuterOutput = pexprOuter->DeriveOutputColumns();
	CColRefSet *pcrsAggOutput = pexprGb->DeriveOutputColumns();
	CColRefSet *pcrsUsed = pexprPrjList->DeriveUsedColumns();
	CColRefSet *pcrsFKey = PcrsFKey(mp, pexprOuter, pexprInner, pexprScalar);

	CColRefSet *pcrsScalarFromOuter =
		GPOS_NEW(mp) CColRefSet(mp, *(pexprScalar->DeriveUsedColumns()));
	pcrsScalarFromOuter->Intersection(pcrsOuterOutput);

	// use minimal grouping columns if they exist, otherwise use all grouping columns
	CColRefSet *pcrsGrpCols = GPOS_NEW(mp) CColRefSet(mp);
	CColRefArray *colref_array = popGbAgg->PdrgpcrMinimal();
	if (nullptr == colref_array)
	{
		colref_array = popGbAgg->Pdrgpcr();
	}
	pcrsGrpCols->Include(colref_array);

	BOOL fCanPush = FCanPushGbAggBelowJoin(pcrsGrpCols, pcrsOuterOutput,
										   pcrsScalarFromOuter, pcrsAggOutput,
										   pcrsUsed, pcrsFKey);

	// cleanup
	CRefCount::SafeRelease(pcrsFKey);
	pcrsScalarFromOuter->Release();

	if (!fCanPush)
	{
		pcrsGrpCols->Release();

		return nullptr;
	}

	// here, we know that grouping columns include FK and all used columns by Gb
	// come only from the outer child of the join;
	// we can safely push Gb to be on top of join's outer child

	popGbAgg->AddRef();
	CLogicalGbAgg *popGbAggNew =
		PopGbAggPushableBelowJoin(mp, popGbAgg, pcrsOuterOutput, pcrsGrpCols);
	pcrsGrpCols->Release();

	pexprOuter->AddRef();
	pexprPrjList->AddRef();
	CExpression *pexprNewGb =
		GPOS_NEW(mp) CExpression(mp, popGbAggNew, pexprOuter, pexprPrjList);

	CExpression *pexprNewOuter = pexprNewGb;
	if (nullptr != pexprSelect)
	{
		// add Select node on top of Gb
		(*pexprSelect)[1]->AddRef();
		pexprNewOuter =
			CUtils::PexprLogicalSelect(mp, pexprNewGb, (*pexprSelect)[1]);
	}

	COperator *popJoin = pexprJoin->Pop();
	popJoin->AddRef();
	pexprInner->AddRef();
	pexprScalar->AddRef();

	return GPOS_NEW(mp)
		CExpression(mp, popJoin, pexprNewOuter, pexprInner, pexprScalar);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PopGbAggPushableBelowJoin
//
//	@doc:
//		Create the Gb operator to be pushed below a join given the original Gb
//		operator, output columns of the join's outer child and grouping cols
//
//---------------------------------------------------------------------------
CLogicalGbAgg *
CXformUtils::PopGbAggPushableBelowJoin(CMemoryPool *mp,
									   CLogicalGbAgg *popGbAggOld,
									   CColRefSet *pcrsOutputOuter,
									   CColRefSet *pcrsGrpCols)
{
	GPOS_ASSERT(nullptr != popGbAggOld);
	GPOS_ASSERT(nullptr != pcrsOutputOuter);
	GPOS_ASSERT(nullptr != pcrsGrpCols);

	CLogicalGbAgg *popGbAggNew = popGbAggOld;
	if (!pcrsOutputOuter->ContainsAll(pcrsGrpCols))
	{
		// we have grouping columns from both join children;
		// we can drop grouping columns from the inner join child since
		// we already have a FK in grouping columns

		popGbAggNew->Release();
		CColRefSet *pcrsGrpColsNew = GPOS_NEW(mp) CColRefSet(mp);
		pcrsGrpColsNew->Include(pcrsGrpCols);
		pcrsGrpColsNew->Intersection(pcrsOutputOuter);
		if (COperator::EopLogicalGbAggDeduplicate == popGbAggOld->Eopid())
		{
			CColRefArray *pdrgpcrKeys =
				CLogicalGbAggDeduplicate::PopConvert(popGbAggOld)
					->PdrgpcrKeys();
			pdrgpcrKeys->AddRef();
			popGbAggNew = GPOS_NEW(mp) CLogicalGbAggDeduplicate(
				mp, pcrsGrpColsNew->Pdrgpcr(mp), popGbAggOld->Egbaggtype(),
				pdrgpcrKeys);
		}
		else
		{
			popGbAggNew = GPOS_NEW(mp) CLogicalGbAgg(
				mp, pcrsGrpColsNew->Pdrgpcr(mp), popGbAggOld->Egbaggtype());
		}
		pcrsGrpColsNew->Release();
	}

	return popGbAggNew;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FCanPushGbAggBelowJoin
//
//	@doc:
//		Check if the preconditions for pushing down Group by through join are
//		satisfied
//---------------------------------------------------------------------------
BOOL
CXformUtils::FCanPushGbAggBelowJoin(CColRefSet *pcrsGrpCols,
									CColRefSet *pcrsJoinOuterChildOutput,
									CColRefSet *pcrsJoinScalarUsedFromOuter,
									CColRefSet *pcrsGrpByOutput,
									CColRefSet *pcrsGrpByUsed,
									CColRefSet *pcrsFKey)
{
	BOOL fGrpByProvidesUsedColumns =
		pcrsGrpByOutput->ContainsAll(pcrsJoinScalarUsedFromOuter);

	BOOL fHasFK = (nullptr != pcrsFKey);
	BOOL fGrpColsContainFK = (fHasFK && pcrsGrpCols->ContainsAll(pcrsFKey));
	BOOL fOutputColsContainUsedCols =
		pcrsJoinOuterChildOutput->ContainsAll(pcrsGrpByUsed);

	if (!fHasFK || !fGrpColsContainFK || !fOutputColsContainUsedCols ||
		!fGrpByProvidesUsedColumns)
	{
		// GrpBy cannot be pushed through join because
		// (1) no FK exists, or
		// (2) FK exists but grouping columns do not include it, or
		// (3) Gb uses columns from both join children, or
		// (4) Gb hides columns required for the join scalar child

		return false;
	}

	return true;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FSameDatatype
//
//	@doc:
//		Check if the input columns of the outer child are the same as the
//		aligned input columns of the each of the inner children.
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FSameDatatype(CColRef2dArray *pdrgpdrgpcrInput)
{
	GPOS_ASSERT(1 < pdrgpdrgpcrInput->Size());

	CColRefArray *pdrgpcrOuter = (*pdrgpdrgpcrInput)[0];

	ULONG ulColIndex = pdrgpcrOuter->Size();
	ULONG child_index = pdrgpdrgpcrInput->Size();
	for (ULONG ulColCounter = 0; ulColCounter < ulColIndex; ulColCounter++)
	{
		CColRef *pcrOuter = (*pdrgpcrOuter)[ulColCounter];

		for (ULONG ulChildCounter = 1; ulChildCounter < child_index;
			 ulChildCounter++)
		{
			CColRefArray *pdrgpcrInnner = (*pdrgpdrgpcrInput)[ulChildCounter];
			CColRef *pcrInner = (*pdrgpcrInnner)[ulColCounter];

			GPOS_ASSERT(pcrOuter != pcrInner);

			if (pcrInner->RetrieveType() != pcrOuter->RetrieveType())
			{
				return false;
			}
		}
	}

	return true;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::ExistentialToAgg
//
//	@doc:
//		Helper for creating Agg expression equivalent to an existential subquery
//
//		Example:
//			For 'exists(select * from r where a = 10)', we produce the following:
//			New Subquery: (select count(*) as cc from r where a = 10)
//			New Scalar: cc > 0
//
//---------------------------------------------------------------------------
void
CXformUtils::ExistentialToAgg(
	CMemoryPool *mp, CExpression *pexprSubquery,
	CExpression **ppexprNewSubquery,  // output argument for new scalar subquery
	CExpression **ppexprNewScalar  // output argument for new scalar expression
)
{
	GPOS_ASSERT(CUtils::FExistentialSubquery(pexprSubquery->Pop()));
	GPOS_ASSERT(nullptr != ppexprNewSubquery);
	GPOS_ASSERT(nullptr != ppexprNewScalar);

	COperator::EOperatorId op_id = pexprSubquery->Pop()->Eopid();
	CExpression *pexprInner = (*pexprSubquery)[0];
	IMDType::ECmpType ecmptype = IMDType::EcmptG;
	if (COperator::EopScalarSubqueryNotExists == op_id)
	{
		ecmptype = IMDType::EcmptEq;
	}

	pexprInner->AddRef();
	CExpression *pexprInnerNew = CUtils::PexprCountStar(mp, pexprInner);
	const CColRef *pcrCount =
		CScalarProjectElement::PopConvert((*(*pexprInnerNew)[1])[0]->Pop())
			->Pcr();

	*ppexprNewSubquery = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CScalarSubquery(mp, pcrCount, true /*fGeneratedByExist*/,
									 false /*fGeneratedByQuantified*/),
		pexprInnerNew);
	*ppexprNewScalar =
		CUtils::PexprCmpWithZero(mp, CUtils::PexprScalarIdent(mp, pcrCount),
								 pcrCount->RetrieveType()->MDId(), ecmptype);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::QuantifiedToAgg
//
//	@doc:
//		Helper for creating Agg expression equivalent to a quantified subquery,
//
//
//---------------------------------------------------------------------------
void
CXformUtils::QuantifiedToAgg(
	CMemoryPool *mp, CExpression *pexprSubquery,
	CExpression **ppexprNewSubquery,  // output argument for new scalar subquery
	CExpression **ppexprNewScalar  // output argument for new scalar expression
)
{
	GPOS_ASSERT(CUtils::FQuantifiedSubquery(pexprSubquery->Pop()));
	GPOS_ASSERT(nullptr != ppexprNewSubquery);
	GPOS_ASSERT(nullptr != ppexprNewScalar);

	if (COperator::EopScalarSubqueryAll == pexprSubquery->Pop()->Eopid())
	{
		return SubqueryAllToAgg(mp, pexprSubquery, ppexprNewSubquery,
								ppexprNewScalar);
	}

	return SubqueryAnyToAgg(mp, pexprSubquery, ppexprNewSubquery,
							ppexprNewScalar);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::SubqueryAnyToAgg
//
//	@doc:
//		Helper for transforming SubqueryAll into aggregate subquery,
//		we need to differentiate between two cases:
//		(1) if subquery predicate uses nullable columns, we may produce null values,
//			this is handeled by adding a null indicator to subquery column, and
//			counting the number of subquery results with null value in subquery column,
//		(2) if subquery predicate does not use nullable columns, we can only produce
//			boolean values,
//
//		Examples:
//
//			- For 'b1 in (select b2 from R)', with b2 not-nullable and is a well-known operator, we produce the following:
//			* New Subquery: (select count(*) as cc from R where b1=b2))
//			* New Scalar: cc > 0
//
//			- For 'b1 in (select b2 from R)', with b2 nullable, we produce the following:
//			* New Subquery: (select Prj_cc from (select count(*), sum(null_indic) from R where b1=b2))
//			where Prj_cc is a project for column cc, defined as follows:
//				if (count(*) == 0), then cc = 0,
//				else if (count(*) == sum(null_indic)), then cc = -1,
//				else cc = count(*)
//			where (-1) indicates that subquery produced a null (this is replaced by NULL in
//			SubqueryHandler when unnesting to LeftApply)
//			* New Scalar: cc > 0
//
//
//---------------------------------------------------------------------------
void
CXformUtils::SubqueryAnyToAgg(
	CMemoryPool *mp, CExpression *pexprSubquery,
	CExpression **ppexprNewSubquery,  // output argument for new scalar subquery
	CExpression **ppexprNewScalar  // output argument for new scalar expression
)
{
	GPOS_ASSERT(CUtils::FQuantifiedSubquery(pexprSubquery->Pop()));
	GPOS_ASSERT(COperator::EopScalarSubqueryAny ==
				pexprSubquery->Pop()->Eopid());
	GPOS_ASSERT(nullptr != ppexprNewSubquery);
	GPOS_ASSERT(nullptr != ppexprNewScalar);

	CExpression *pexprInner = (*pexprSubquery)[0];

	// build subquery quantified comparison
	CExpression *pexprResult = nullptr;
	CSubqueryHandler sh(mp, false /* fEnforceCorrelatedApply */);
	CExpression *pexprSubqPred =
		sh.PexprSubqueryPred(pexprInner, pexprSubquery, &pexprResult,
							 CSubqueryHandler::EsqctxtFilter);
	CScalarCmp *scalarCmp = CScalarCmp::PopConvert(pexprSubqPred->Pop());

	GPOS_ASSERT(nullptr != scalarCmp);

	const CColRef *pcrSubq =
		CScalarSubqueryQuantified::PopConvert(pexprSubquery->Pop())->Pcr();
	BOOL fCanEvaluateToNull =
		(CUtils::FUsesNullableCol(mp, pexprSubqPred, pexprResult) ||
		 !CPredicateUtils::FBuiltInComparisonIsVeryStrict(scalarCmp->MdIdOp()));

	CExpression *pexprInnerNew = nullptr;
	pexprInner->AddRef();
	if (fCanEvaluateToNull)
	{
		// TODO: change this to <pexprSubqPred> is not false, get rid of pexprNullIndicator
		// add a null indicator
		CExpression *pexprNullIndicator =
			PexprNullIndicator(mp, CUtils::PexprScalarIdent(mp, pcrSubq));
		CExpression *pexprPrj =
			CUtils::PexprAddProjection(mp, pexprResult, pexprNullIndicator);
		pexprResult = pexprPrj;

		// add disjunction with is not null check
		CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
		pdrgpexpr->Append(pexprSubqPred);
		pdrgpexpr->Append(
			CUtils::PexprIsNull(mp, CUtils::PexprScalarIdent(mp, pcrSubq)));

		pexprSubqPred = CPredicateUtils::PexprDisjunction(mp, pdrgpexpr);
	}

	CExpression *pexprSelect =
		CUtils::PexprLogicalSelect(mp, pexprResult, pexprSubqPred);
	if (fCanEvaluateToNull)
	{
		const CColRef *pcrNullIndicator =
			CScalarProjectElement::PopConvert(
				(*(*(*pexprSelect)[0])[1])[0]->Pop())
				->Pcr();
		pexprInnerNew =
			CUtils::PexprCountStarAndSum(mp, pcrNullIndicator, pexprSelect);
		const CColRef *pcrCount =
			CScalarProjectElement::PopConvert((*(*pexprInnerNew)[1])[0]->Pop())
				->Pcr();
		const CColRef *pcrSum =
			CScalarProjectElement::PopConvert((*(*pexprInnerNew)[1])[1]->Pop())
				->Pcr();

		CExpression *pexprScalarIdentCount =
			CUtils::PexprScalarIdent(mp, pcrCount);
		CExpression *pexprCountEqZero = CUtils::PexprCmpWithZero(
			mp, pexprScalarIdentCount,
			CScalarIdent::PopConvert(pexprScalarIdentCount->Pop())->MdidType(),
			IMDType::EcmptEq);
		CExpression *pexprCountEqSum =
			CUtils::PexprScalarEqCmp(mp, pcrCount, pcrSum);

		CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
		const IMDTypeInt8 *pmdtypeint8 = md_accessor->PtMDType<IMDTypeInt8>();
		IMDId *pmdidInt8 = pmdtypeint8->MDId();
		pmdidInt8->AddRef();
		pmdidInt8->AddRef();

		CExpression *pexprProjected = GPOS_NEW(mp) CExpression(
			mp, GPOS_NEW(mp) CScalarIf(mp, pmdidInt8), pexprCountEqZero,
			CUtils::PexprScalarConstInt8(mp, 0 /*val*/),
			GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CScalarIf(mp, pmdidInt8), pexprCountEqSum,
				CUtils::PexprScalarConstInt8(mp, -1 /*val*/),
				CUtils::PexprScalarIdent(mp, pcrCount)));
		CExpression *pexprPrj =
			CUtils::PexprAddProjection(mp, pexprInnerNew, pexprProjected);

		const CColRef *pcrSubquery =
			CScalarProjectElement::PopConvert((*(*pexprPrj)[1])[0]->Pop())
				->Pcr();
		*ppexprNewSubquery = GPOS_NEW(mp) CExpression(
			mp,
			GPOS_NEW(mp)
				CScalarSubquery(mp, pcrSubquery, false /*fGeneratedByExist*/,
								true /*fGeneratedByQuantified*/),
			pexprPrj);
		*ppexprNewScalar = CUtils::PexprCmpWithZero(
			mp, CUtils::PexprScalarIdent(mp, pcrSubquery),
			pcrSubquery->RetrieveType()->MDId(), IMDType::EcmptG);
	}
	else
	{
		// replace <col1> <op> ANY (select <col2> from SQ) with
		//         (select count(*) from (select )) > 0
		pexprInnerNew = CUtils::PexprCountStar(mp, pexprSelect);
		const CColRef *pcrCount =
			CScalarProjectElement::PopConvert((*(*pexprInnerNew)[1])[0]->Pop())
				->Pcr();

		*ppexprNewSubquery = GPOS_NEW(mp) CExpression(
			mp,
			GPOS_NEW(mp)
				CScalarSubquery(mp, pcrCount, false /*fGeneratedByExist*/,
								true /*fGeneratedByQuantified*/),
			pexprInnerNew);
		*ppexprNewScalar = CUtils::PexprCmpWithZero(
			mp, CUtils::PexprScalarIdent(mp, pcrCount),
			pcrCount->RetrieveType()->MDId(), IMDType::EcmptG);
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::SubqueryAllToAgg
//
//	@doc:
//		Helper for transforming SubqueryAll into aggregate subquery,
//		we generate aggregate expressions that compute the following values:
//		- N: number of null values returned by evaluating inner expression
//		- S: number of inner values matching outer value
//		the generated subquery returns a Boolean result generated by the following
//		nested-if statement:
//
//			if (inner is empty)
//				return true
//			else if (N > 0)
//				return null
//			else if (outer value is null)
//				return null
//			else if (S == 0)
//				return true
//			else
//				return false
//
//
//---------------------------------------------------------------------------
void
CXformUtils::SubqueryAllToAgg(
	CMemoryPool *mp, CExpression *pexprSubquery,
	CExpression **ppexprNewSubquery,  // output argument for new scalar subquery
	CExpression **ppexprNewScalar  // output argument for new scalar expression
)
{
	GPOS_ASSERT(CUtils::FQuantifiedSubquery(pexprSubquery->Pop()));
	GPOS_ASSERT(COperator::EopScalarSubqueryAll ==
				pexprSubquery->Pop()->Eopid());
	GPOS_ASSERT(nullptr != ppexprNewSubquery);
	GPOS_ASSERT(nullptr != ppexprNewScalar);

	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();

	CExpression *pexprInner = (*pexprSubquery)[0];
	CExpression *pexprScalarOuter = (*pexprSubquery)[1];
	CExpression *pexprSubqPred = PexprInversePred(mp, pexprSubquery);

	// generate subquery test expression
	const IMDTypeInt4 *pmdtypeint4 = md_accessor->PtMDType<IMDTypeInt4>();
	IMDId *pmdidInt4 = pmdtypeint4->MDId();
	pmdidInt4->AddRef();
	CExpression *pexprSubqTest = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarIf(mp, pmdidInt4), pexprSubqPred,
					CUtils::PexprScalarConstInt4(mp, 1 /*val*/),
					CUtils::PexprScalarConstInt4(mp, 0 /*val*/));

	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	pdrgpexpr->Append(pexprSubqTest);

	// generate null indicator for inner expression
	const CColRef *pcrSubq =
		CScalarSubqueryQuantified::PopConvert(pexprSubquery->Pop())->Pcr();
	CExpression *pexprInnerNullIndicator =
		PexprNullIndicator(mp, CUtils::PexprScalarIdent(mp, pcrSubq));
	pdrgpexpr->Append(pexprInnerNullIndicator);

	// add generated expression as projected nodes
	pexprInner->AddRef();
	CExpression *pexprPrj =
		CUtils::PexprAddProjection(mp, pexprInner, pdrgpexpr);
	pdrgpexpr->Release();

	// generate a group by expression with sum(subquery-test) and sum(inner null indicator) aggreagtes
	CColRefArray *colref_array = GPOS_NEW(mp) CColRefArray(mp);
	CColRef *pcrSubqTest =
		CScalarProjectElement::PopConvert((*(*pexprPrj)[1])[0]->Pop())->Pcr();
	CColRef *pcrInnerNullTest =
		CScalarProjectElement::PopConvert((*(*pexprPrj)[1])[1]->Pop())->Pcr();
	colref_array->Append(pcrSubqTest);
	colref_array->Append(pcrInnerNullTest);
	CExpression *pexprGbAggSum =
		CUtils::PexprGbAggSum(mp, pexprPrj, colref_array);
	colref_array->Release();

	// generate helper test expressions
	const CColRef *pcrSum =
		CScalarProjectElement::PopConvert((*(*pexprGbAggSum)[1])[0]->Pop())
			->Pcr();
	const CColRef *pcrSumNulls =
		CScalarProjectElement::PopConvert((*(*pexprGbAggSum)[1])[1]->Pop())
			->Pcr();
	CExpression *pexprScalarIdentSum = CUtils::PexprScalarIdent(mp, pcrSum);
	CExpression *pexprScalarIdentSumNulls =
		CUtils::PexprScalarIdent(mp, pcrSumNulls);

	CExpression *pexprSumTest = CUtils::PexprCmpWithZero(
		mp, pexprScalarIdentSum,
		CScalarIdent::PopConvert(pexprScalarIdentSum->Pop())->MdidType(),
		IMDType::EcmptEq);
	pexprScalarIdentSum->AddRef();
	CExpression *pexprIsInnerEmpty =
		CUtils::PexprIsNull(mp, pexprScalarIdentSum);
	CExpression *pexprInnerHasNulls = CUtils::PexprCmpWithZero(
		mp, pexprScalarIdentSumNulls,
		CScalarIdent::PopConvert(pexprScalarIdentSumNulls->Pop())->MdidType(),
		IMDType::EcmptG);
	pexprScalarOuter->AddRef();
	CExpression *pexprIsOuterNull = CUtils::PexprIsNull(mp, pexprScalarOuter);

	// generate the main scalar if that will produce subquery result
	const IMDTypeBool *pmdtypebool = md_accessor->PtMDType<IMDTypeBool>();
	IMDId *pmdidBool = pmdtypebool->MDId();
	pmdidBool->AddRef();
	pmdidBool->AddRef();
	pmdidBool->AddRef();
	pmdidBool->AddRef();
	pexprPrj = GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CScalarIf(mp, pmdidBool), pexprIsInnerEmpty,
		CUtils::PexprScalarConstBool(
			mp, true /*value*/),  // if inner is empty, return true
		GPOS_NEW(mp) CExpression(
			mp, GPOS_NEW(mp) CScalarIf(mp, pmdidBool), pexprInnerHasNulls,
			CUtils::PexprScalarConstBool(
				mp, false /*value*/,
				true /*is_null*/),	// if inner produced null values, return null
			GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CScalarIf(mp, pmdidBool), pexprIsOuterNull,
				CUtils::PexprScalarConstBool(
					mp, false /*value*/,
					true /*is_null*/),	// if outer value is null, return null
				GPOS_NEW(mp) CExpression(
					mp, GPOS_NEW(mp) CScalarIf(mp, pmdidBool),
					pexprSumTest,  // otherwise, test number of inner values that match outer value
					CUtils::PexprScalarConstBool(mp,
												 true /*value*/),  // no matches
					CUtils::PexprScalarConstBool(
						mp, false /*value*/)  // at least one match
					))));

	CExpression *pexprProjected =
		CUtils::PexprAddProjection(mp, pexprGbAggSum, pexprPrj);

	const CColRef *pcrSubquery =
		CScalarProjectElement::PopConvert((*(*pexprProjected)[1])[0]->Pop())
			->Pcr();
	*ppexprNewSubquery = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp)
			CScalarSubquery(mp, pcrSubquery, false /*fGeneratedByExist*/,
							true /*fGeneratedByQuantified*/),
		pexprProjected);
	*ppexprNewScalar = CUtils::PexprScalarCmp(
		mp, CUtils::PexprScalarIdent(mp, pcrSubquery),
		CUtils::PexprScalarConstBool(mp, true /*value*/), IMDType::EcmptEq);
}


//---------------------------------------------------------------------------
// CXformUtils::PexprSeparateSubqueryPreds
//
// Helper function to separate subquery predicates in a top Select node.
// Transforms a join expression join(<logical children>, <expr with SQ>)
// into select(join(<logical children>, <expr>), <subquery preds>).
// Returns NULL if there are no subqueries in the inner join predicates.
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprSeparateSubqueryPreds(CMemoryPool *mp, CExpression *pexpr)
{
	COperator::EOperatorId op_id = pexpr->Pop()->Eopid();
	GPOS_ASSERT(COperator::EopLogicalInnerJoin == op_id ||
				COperator::EopLogicalNAryJoin == op_id);

	// split scalar expression into a conjunction of predicates with and without
	// subqueries
	const ULONG arity = pexpr->Arity();
	CExpression *pexprScalar = (*pexpr)[arity - 1];
	CLogicalNAryJoin *naryLOJOp =
		CLogicalNAryJoin::PopConvertNAryLOJ(pexpr->Pop());
	CExpression *innerJoinPreds = pexprScalar;
	if (nullptr != naryLOJOp)
	{
		innerJoinPreds = naryLOJOp->GetInnerJoinPreds(pexpr);
	}
	CExpressionArray *pdrgpexprConjuncts =
		CPredicateUtils::PdrgpexprConjuncts(mp, innerJoinPreds);
	CExpressionArray *pdrgpexprSQ = GPOS_NEW(mp) CExpressionArray(mp);
	CExpressionArray *pdrgpexprNonSQ = GPOS_NEW(mp) CExpressionArray(mp);

	const ULONG ulConjuncts = pdrgpexprConjuncts->Size();
	for (ULONG ul = 0; ul < ulConjuncts; ul++)
	{
		CExpression *pexprConj = (*pdrgpexprConjuncts)[ul];
		pexprConj->AddRef();

		if (pexprConj->DeriveHasSubquery())
		{
			pdrgpexprSQ->Append(pexprConj);
		}
		else
		{
			pdrgpexprNonSQ->Append(pexprConj);
		}
	}

	pdrgpexprConjuncts->Release();

	if (0 == pdrgpexprSQ->Size())
	{
		// no subqueries found in inner join predicates, they must be in the LOJ preds
		GPOS_ASSERT(nullptr != naryLOJOp);
		pdrgpexprSQ->Release();
		pdrgpexprNonSQ->Release();

		return nullptr;
	}

	// build children array from logical children
	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	for (ULONG ul = 0; ul < arity - 1; ul++)
	{
		CExpression *pexprChild = (*pexpr)[ul];
		pexprChild->AddRef();
		pdrgpexpr->Append(pexprChild);
	}

	// build a new join with the new non-subquery predicates
	COperator *popJoin = nullptr;

	if (nullptr == naryLOJOp)
	{
		if (COperator::EopLogicalInnerJoin == op_id)
		{
			popJoin = GPOS_NEW(mp) CLogicalInnerJoin(mp);
		}
		else
		{
			popJoin = GPOS_NEW(mp) CLogicalNAryJoin(mp);
		}
		pdrgpexpr->Append(
			CPredicateUtils::PexprConjunction(mp, pdrgpexprNonSQ));
	}
	else
	{
		// nary LOJ, make sure to include the indexes assigning children
		// to LOJs and to preserve the CScalarNAryJoinPredList
		ULongPtrArray *childIndexes = naryLOJOp->GetLojChildPredIndexes();

		childIndexes->AddRef();

		popJoin = GPOS_NEW(mp) CLogicalNAryJoin(mp, childIndexes);

		pdrgpexpr->Append(naryLOJOp->ReplaceInnerJoinPredicates(
			mp, pexprScalar,
			CPredicateUtils::PexprConjunction(mp, pdrgpexprNonSQ)));
	}

	CExpression *pexprJoin = GPOS_NEW(mp) CExpression(mp, popJoin, pdrgpexpr);

	// return a Select node with a conjunction of subquery predicates
	return CUtils::PexprLogicalSelect(
		mp, pexprJoin, CPredicateUtils::PexprConjunction(mp, pdrgpexprSQ));
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprInversePred
//
//	@doc:
//		Helper for creating the inverse of the predicate used by
//		subquery ALL
//
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprInversePred(CMemoryPool *mp, CExpression *pexprSubquery)
{
	// get the scalar child of subquery
	CScalarSubqueryAll *popSqAll =
		CScalarSubqueryAll::PopConvert(pexprSubquery->Pop());
	CExpression *pexprScalar = (*pexprSubquery)[1];
	const CColRef *colref = popSqAll->Pcr();
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();

	// get mdid and name of the inverse of the comparison operator used by subquery
	IMDId *mdid_op = popSqAll->MdIdOp();
	IMDId *pmdidInverseOp =
		md_accessor->RetrieveScOp(mdid_op)->GetInverseOpMdid();
	const CWStringConst *pstrFirst =
		md_accessor->RetrieveScOp(pmdidInverseOp)->Mdname().GetMDName();

	// generate a predicate for the inversion of the comparison involved in the subquery
	pexprScalar->AddRef();
	pmdidInverseOp->AddRef();

	return CUtils::PexprScalarCmp(mp, pexprScalar, colref, *pstrFirst,
								  pmdidInverseOp);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprNullIndicator
//
//	@doc:
//		Helper for creating a null indicator
//		Creates an expression case when <pexpr> is null then 1 else 0 end
//
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprNullIndicator(CMemoryPool *mp, CExpression *pexpr)
{
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();

	CExpression *pexprIsNull = CUtils::PexprIsNull(mp, pexpr);
	const IMDTypeInt4 *pmdtypeint4 = md_accessor->PtMDType<IMDTypeInt4>();
	IMDId *mdid = pmdtypeint4->MDId();
	mdid->AddRef();
	return GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarIf(mp, mdid), pexprIsNull,
					CUtils::PexprScalarConstInt4(mp, 1 /*val*/),
					CUtils::PexprScalarConstInt4(mp, 0 /*val*/));
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprLogicalDMLOverProject
//
//	@doc:
// 		Create a logical DML on top of a project
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprLogicalDMLOverProject(CMemoryPool *mp,
										CExpression *pexprChild,
										CLogicalDML::EDMLOperator edmlop,
										CTableDescriptor *ptabdesc,
										CColRefArray *colref_array,
										CColRef *pcrCtid, CColRef *pcrSegmentId)
{
	GPOS_ASSERT(CLogicalDML::EdmlInsert == edmlop ||
				CLogicalDML::EdmlDelete == edmlop);
	INT val = CScalarDMLAction::EdmlactionInsert;
	if (CLogicalDML::EdmlDelete == edmlop)
	{
		val = CScalarDMLAction::EdmlactionDelete;
	}

	// new expressions to project
	CExpression *pexprProject = nullptr;
	CColRef *pcrAction = nullptr;

	CExpressionArray *pdrgpexprProjected = GPOS_NEW(mp) CExpressionArray(mp);
	// generate one project node with new columns: action
	pdrgpexprProjected->Append(CUtils::PexprScalarConstInt4(mp, val));

	pexprProject =
		CUtils::PexprAddProjection(mp, pexprChild, pdrgpexprProjected);
	pdrgpexprProjected->Release();

	CExpression *pexprPrL = (*pexprProject)[1];
	pcrAction = CUtils::PcrFromProjElem((*pexprPrL)[0]);

	GPOS_ASSERT(nullptr != pcrAction);

	if (CLogicalDML::EdmlInsert == edmlop)
	{
		// add assert for check constraints and nullness checks if needed
		COptimizerConfig *optimizer_config =
			COptCtxt::PoctxtFromTLS()->GetOptimizerConfig();
		if (optimizer_config->GetHint()->FEnforceConstraintsOnDML())
		{
			pexprProject = PexprAssertConstraints(mp, pexprProject, ptabdesc,
												  colref_array);
		}
	}

	CExpression *pexprDML = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CLogicalDML(mp, edmlop, ptabdesc, colref_array,
								 GPOS_NEW(mp) CBitSet(mp) /*pbsModified*/,
								 pcrAction, pcrCtid, pcrSegmentId, true),
		pexprProject);

	CExpression *pexprOutput = pexprDML;

	return pexprOutput;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprAssertNotNull
//
//	@doc:
//		Construct an assert on top of the given expression for nullness checks
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprAssertNotNull(CMemoryPool *mp, CExpression *pexprChild,
								CTableDescriptor *ptabdesc,
								CColRefArray *colref_array)
{
	CColumnDescriptorArray *pdrgpcoldesc = ptabdesc->Pdrgpcoldesc();

	const ULONG num_cols = pdrgpcoldesc->Size();
	CColRefSet *pcrsNotNull = pexprChild->DeriveNotNullColumns();

	CExpressionArray *pdrgpexprAssertConstraints =
		GPOS_NEW(mp) CExpressionArray(mp);

	for (ULONG ul = 0; ul < num_cols; ul++)
	{
		CColumnDescriptor *pcoldesc = (*pdrgpcoldesc)[ul];
		if (pcoldesc->IsNullable() || pcoldesc->IsSystemColumn())
		{
			// target column is nullable or it's a system column: no need to check for NULL
			continue;
		}

		CColRef *colref = (*colref_array)[ul];

		if (pcrsNotNull->FMember(colref))
		{
			// source column not nullable: no need to check for NULL
			continue;
		}

		// add not null check for current column
		CExpression *pexprNotNull =
			CUtils::PexprIsNotNull(mp, CUtils::PexprScalarIdent(mp, colref));

		CWStringConst *pstrErrorMsg =
			PstrErrorMessage(mp, gpos::CException::ExmaSQL,
							 gpos::CException::ExmiSQLNotNullViolation,
							 pcoldesc->Name().Pstr()->GetBuffer(),
							 ptabdesc->Name().Pstr()->GetBuffer());

		CExpression *pexprAssertConstraint = GPOS_NEW(mp) CExpression(
			mp, GPOS_NEW(mp) CScalarAssertConstraint(mp, pstrErrorMsg),
			pexprNotNull);

		pdrgpexprAssertConstraints->Append(pexprAssertConstraint);
	}

	if (0 == pdrgpexprAssertConstraints->Size())
	{
		pdrgpexprAssertConstraints->Release();
		return pexprChild;
	}

	CExpression *pexprAssertPredicate = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarAssertConstraintList(mp),
					pdrgpexprAssertConstraints);

	CLogicalAssert *popAssert = GPOS_NEW(mp) CLogicalAssert(
		mp, GPOS_NEW(mp) CException(gpos::CException::ExmaSQL,
									gpos::CException::ExmiSQLNotNullViolation));

	return GPOS_NEW(mp)
		CExpression(mp, popAssert, pexprChild, pexprAssertPredicate);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PdrgpexprPartFilters
//
//	@doc:
// 		Return partition filter expressions for a DML operation given a table
//		descriptor and the column references seen by this DML
//
//---------------------------------------------------------------------------
CExpressionArray *
CXformUtils::PdrgpexprPartEqFilters(CMemoryPool *mp, CTableDescriptor *ptabdesc,
									CColRefArray *pdrgpcrSource)
{
	GPOS_ASSERT(nullptr != ptabdesc);
	GPOS_ASSERT(nullptr != pdrgpcrSource);

	const ULongPtrArray *pdrgpulPart = ptabdesc->PdrgpulPart();

	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);

	const ULONG ulPartKeys = pdrgpulPart->Size();
	GPOS_ASSERT(0 < ulPartKeys);
	GPOS_ASSERT(pdrgpcrSource->Size() >= ulPartKeys);

	for (ULONG ul = 0; ul < ulPartKeys; ul++)
	{
		ULONG *pulPartKey = (*pdrgpulPart)[ul];
		CColRef *colref = (*pdrgpcrSource)[*pulPartKey];
		pdrgpexpr->Append(CUtils::PexprScalarIdent(mp, colref));
	}

	return pdrgpexpr;
}

//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::PexprAssertConstraints
//
//      @doc:
//          Construct an assert on top of the given expression for check constraints
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprAssertConstraints(CMemoryPool *mp, CExpression *pexprChild,
									CTableDescriptor *ptabdesc,
									CColRefArray *colref_array)
{
	CExpression *pexprAssertNotNull =
		PexprAssertNotNull(mp, pexprChild, ptabdesc, colref_array);

	return PexprAssertCheckConstraints(mp, pexprAssertNotNull, ptabdesc,
									   colref_array);
}

//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::PexprAssertCheckConstraints
//
//      @doc:
//          Construct an assert on top of the given expression for check constraints
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprAssertCheckConstraints(CMemoryPool *mp,
										 CExpression *pexprChild,
										 CTableDescriptor *ptabdesc,
										 CColRefArray *colref_array)
{
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	const IMDRelation *pmdrel = md_accessor->RetrieveRel(ptabdesc->MDId());

	const ULONG ulCheckConstraint = pmdrel->CheckConstraintCount();
	if (0 < ulCheckConstraint)
	{
		CExpressionArray *pdrgpexprAssertConstraints =
			GPOS_NEW(mp) CExpressionArray(mp);

		for (ULONG ul = 0; ul < ulCheckConstraint; ul++)
		{
			IMDId *pmdidCheckConstraint = pmdrel->CheckConstraintMDidAt(ul);
			const IMDCheckConstraint *pmdCheckConstraint =
				md_accessor->RetrieveCheckConstraints(pmdidCheckConstraint);

			// extract the check constraint expression
			CExpression *pexprCheckConstraint =
				pmdCheckConstraint->GetCheckConstraintExpr(mp, md_accessor,
														   colref_array);

			// A table check constraint is satisfied if and only if the specified <search condition>
			// evaluates to True or Unknown for every row of the table to which it applies.
			// Add an "is not false" expression on top to handle such scenarios
			CExpression *pexprIsNotFalse =
				CUtils::PexprIsNotFalse(mp, pexprCheckConstraint);
			CWStringConst *pstrErrMsg = PstrErrorMessage(
				mp, gpos::CException::ExmaSQL,
				gpos::CException::ExmiSQLCheckConstraintViolation,
				pmdCheckConstraint->Mdname().GetMDName()->GetBuffer(),
				ptabdesc->Name().Pstr()->GetBuffer());
			CExpression *pexprAssertConstraint = GPOS_NEW(mp) CExpression(
				mp, GPOS_NEW(mp) CScalarAssertConstraint(mp, pstrErrMsg),
				pexprIsNotFalse);

			pdrgpexprAssertConstraints->Append(pexprAssertConstraint);
		}

		CExpression *pexprAssertPredicate = GPOS_NEW(mp)
			CExpression(mp, GPOS_NEW(mp) CScalarAssertConstraintList(mp),
						pdrgpexprAssertConstraints);

		CLogicalAssert *popAssert = GPOS_NEW(mp) CLogicalAssert(
			mp, GPOS_NEW(mp) CException(
					gpos::CException::ExmaSQL,
					gpos::CException::ExmiSQLCheckConstraintViolation));


		return GPOS_NEW(mp)
			CExpression(mp, popAssert, pexprChild, pexprAssertPredicate);
	}

	return pexprChild;
}


//---------------------------------------------------------------------------
//   @function:
//		CXformUtils::FSplitAggXform
//
//   @doc:
//      Check if given xform is an Agg splitting xform
//
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FSplitAggXform(CXform::EXformId exfid)
{
	return CXform::ExfSplitGbAgg == exfid || CXform::ExfSplitDQA == exfid ||
		   CXform::ExfSplitGbAggDedup == exfid || CXform::ExfEagerAgg == exfid;
}

BOOL
CXformUtils::FAggGenBySplitDQAXform(CExpression *pexprAgg)
{
	CGroupExpression *pgexprOrigin = pexprAgg->Pgexpr();
	if (nullptr != pgexprOrigin)
	{
		return CXform::ExfSplitDQA == pgexprOrigin->ExfidOrigin();
	}

	return false;
}

// Check if given expression is a multi-stage Agg based on agg type
// or origin xform
BOOL
CXformUtils::FMultiStageAgg(CExpression *pexprAgg)
{
	GPOS_ASSERT(nullptr != pexprAgg);
	GPOS_ASSERT(COperator::EopLogicalGbAgg == pexprAgg->Pop()->Eopid() ||
				COperator::EopLogicalGbAggDeduplicate ==
					pexprAgg->Pop()->Eopid());

	CLogicalGbAgg *popAgg = CLogicalGbAgg::PopConvert(pexprAgg->Pop());
	if (COperator::EgbaggtypeGlobal != popAgg->Egbaggtype())
	{
		// a non-global agg is a multi-stage agg
		return true;
	}

	// check xform lineage
	BOOL fMultiStage = false;
	CGroupExpression *pgexprOrigin = pexprAgg->Pgexpr();
	while (nullptr != pgexprOrigin && !fMultiStage)
	{
		fMultiStage = FSplitAggXform(pgexprOrigin->ExfidOrigin());
		pgexprOrigin = pgexprOrigin->PgexprOrigin();
	}

	return fMultiStage;
}

BOOL
CXformUtils::FLocalAggCreatedByEagerAggXform(CExpression *pexprAgg)
{
	GPOS_ASSERT(nullptr != pexprAgg);
	GPOS_ASSERT(COperator::EopLogicalGbAgg == pexprAgg->Pop()->Eopid() ||
				COperator::EopLogicalGbAggDeduplicate ==
					pexprAgg->Pop()->Eopid());

	CLogicalGbAgg *popAgg = CLogicalGbAgg::PopConvert(pexprAgg->Pop());
	if (COperator::EgbaggtypeLocal != popAgg->Egbaggtype())
	{
		return false;
	}

	CGroupExpression *pgexprOrigin = pexprAgg->Pgexpr();
	// check xform lineage
	BOOL is_eager_agg = false;
	while (nullptr != pgexprOrigin && !is_eager_agg)
	{
		// parse all expressions in group to check if any was created by CXformEagerAgg
		is_eager_agg = CXform::ExfEagerAgg == pgexprOrigin->ExfidOrigin();
		pgexprOrigin = pgexprOrigin->PgexprOrigin();
	}

	return is_eager_agg;
}



//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::FXformInArray
//
//      @doc:
//          Check if given xform id is in the given array of xforms
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FXformInArray(CXform::EXformId exfid,
						   const CXform::EXformId rgXforms[], ULONG ulXforms)
{
	for (ULONG ul = 0; ul < ulXforms; ul++)
	{
		if (rgXforms[ul] == exfid)
		{
			return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::FDeriveStatsBeforeXform
//
//      @doc:
//          Return true if stats derivation is needed for this xform
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FDeriveStatsBeforeXform(CXform *pxform)
{
	GPOS_ASSERT(nullptr != pxform);

	return pxform->FExploration() &&
		   CXformExploration::Pxformexp(pxform)->FNeedsStats();
}

//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::FSubqueryDecorrelation
//
//      @doc:
//          Check if xform is a subquery decorrelation xform
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FSubqueryDecorrelation(CXform *pxform)
{
	GPOS_ASSERT(nullptr != pxform);

	return pxform->FExploration() &&
		   CXformExploration::Pxformexp(pxform)->FApplyDecorrelating();
}


//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::FSubqueryUnnesting
//
//      @doc:
//          Check if xform is a subquery unnesting xform
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FSubqueryUnnesting(CXform *pxform)
{
	GPOS_ASSERT(nullptr != pxform);

	return pxform->FExploration() &&
		   CXformExploration::Pxformexp(pxform)->FSubqueryUnnesting();
}

//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::FApplyToNextBinding
//
//      @doc:
//         Check if xform should be applied to the next binding
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FApplyToNextBinding(
	CXform *pxform,
	CExpression *pexprLastBinding  // last extracted xform pattern
)
{
	GPOS_ASSERT(nullptr != pxform);

	if (FSubqueryDecorrelation(pxform))
	{
		// if last binding is free from Subquery or Apply operators, we do not
		// need to apply the xform further
		return CUtils::FHasSubqueryOrApply(pexprLastBinding,
										   false /*fCheckRoot*/) ||
			   CUtils::FHasCorrelatedApply(pexprLastBinding,
										   false /*fCheckRoot*/);
	}

	// set of transformations that should be applied once
	CXform::EXformId rgXforms[] = {
		CXform::ExfJoinAssociativity,
		CXform::ExfExpandFullOuterJoin,
		CXform::ExfUnnestTVF,
		CXform::ExfLeftSemiJoin2CrossProduct,
		CXform::ExfLeftAntiSemiJoin2CrossProduct,
		CXform::ExfLeftAntiSemiJoinNotIn2CrossProduct,
	};

	CXform::EXformId exfid = pxform->Exfid();

	BOOL fApplyOnce = FSubqueryUnnesting(pxform) ||
					  FXformInArray(exfid, rgXforms, GPOS_ARRAY_SIZE(rgXforms));

	return !fApplyOnce;
}

//---------------------------------------------------------------------------
//      @function:
//              CXformUtils::PstrErrorMessage
//
//      @doc:
//          Compute an error message for given exception type
//
//---------------------------------------------------------------------------
CWStringConst *
CXformUtils::PstrErrorMessage(CMemoryPool *mp, ULONG major, ULONG minor, ...)
{
	WCHAR wsz[1024];
	CWStringStatic str(wsz, 1024);

	// manufacture actual exception object
	CException exc(major, minor);

	// during bootstrap there's no context object otherwise, record
	// all details in the context object
	if (nullptr != ITask::Self())
	{
		VA_LIST valist;
		VA_START(valist, minor);

		ELocale eloc = ITask::Self()->Locale();
		CMessage *pmsg =
			CMessageRepository::GetMessageRepository()->LookupMessage(exc,
																	  eloc);
		pmsg->Format(&str, valist);

		VA_END(valist);
	}

	return GPOS_NEW(mp) CWStringConst(mp, str.GetBuffer());
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PdrgpcrIndexKeys
//
//	@doc:
//		Return the array of columns from the given array of columns which appear
//		in the index key columns
//
//---------------------------------------------------------------------------
CColRefArray *
CXformUtils::PdrgpcrIndexKeys(CMemoryPool *mp, CColRefArray *colref_array,
							  const IMDIndex *pmdindex,
							  const IMDRelation *pmdrel)
{
	return PdrgpcrIndexColumns(mp, colref_array, pmdindex, pmdrel);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PcrsIndexKeys
//
//	@doc:
//		Return the set of columns from the given array of columns which appear
//		in the index key columns
//
//---------------------------------------------------------------------------
CColRefSet *
CXformUtils::PcrsIndexKeys(CMemoryPool *mp, CColRefArray *colref_array,
						   const IMDIndex *pmdindex, const IMDRelation *pmdrel)
{
	return PcrsIndexColumns(mp, colref_array, pmdindex, pmdrel);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PcrsIndexColumns
//
//	@doc:
//		Return the set of columns from the given array of columns which appear
//		in the index columns of the specified type (included / key)
//
//---------------------------------------------------------------------------
CColRefSet *
CXformUtils::PcrsIndexColumns(CMemoryPool *mp, CColRefArray *colref_array,
							  const IMDIndex *pmdindex,
							  const IMDRelation *pmdrel)
{
	CColRefArray *pdrgpcrIndexColumns =
		PdrgpcrIndexColumns(mp, colref_array, pmdindex, pmdrel);
	CColRefSet *pcrsCols = GPOS_NEW(mp) CColRefSet(mp, pdrgpcrIndexColumns);

	pdrgpcrIndexColumns->Release();

	return pcrsCols;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PdrgpcrIndexColumns
//
//	@doc:
//		Return the set of columns from the given array of columns which are
//		retunable through the index (to determine index-only scan capable)
//
//---------------------------------------------------------------------------
CColRefSet *
CXformUtils::PcrsIndexReturnableColumns(CMemoryPool *mp,
										CColRefArray *colref_array,
										const IMDIndex *pmdindex,
										const IMDRelation *pmdrel)
{
	CColRefSet *pcrsCols = GPOS_NEW(mp) CColRefSet(mp);

	// returnable columns
	for (ULONG ul = 0; ul < pmdindex->ReturnableCols(); ul++)
	{
		ULONG ulPos = pmdindex->ReturnableColAt(ul);

		ULONG ulPosNonDropped = pmdrel->NonDroppedColAt(ulPos);
		GPOS_ASSERT(gpos::ulong_max != ulPosNonDropped);
		GPOS_ASSERT(ulPosNonDropped < colref_array->Size());
		CColRef *colref = (*colref_array)[ulPosNonDropped];
		pcrsCols->Include(colref);
	}

	return pcrsCols;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PdrgpcrIndexColumns
//
//	@doc:
//		Return the ordered list of columns from the given array of columns which
//		appear in the index columns of the specified type (included / key)
//
//---------------------------------------------------------------------------
CColRefArray *
CXformUtils::PdrgpcrIndexColumns(CMemoryPool *mp, CColRefArray *colref_array,
								 const IMDIndex *pmdindex,
								 const IMDRelation *pmdrel)
{
	CColRefArray *pdrgpcrIndex = GPOS_NEW(mp) CColRefArray(mp);

	// key columns
	for (ULONG ul = 0; ul < pmdindex->Keys(); ul++)
	{
		ULONG ulPos = pmdindex->KeyAt(ul);

		ULONG ulPosNonDropped = pmdrel->NonDroppedColAt(ulPos);
		GPOS_ASSERT(gpos::ulong_max != ulPosNonDropped);
		GPOS_ASSERT(ulPosNonDropped < colref_array->Size());
		CColRef *colref = (*colref_array)[ulPosNonDropped];
		pdrgpcrIndex->Append(colref);
	}

	// included columns
	for (ULONG ul = 0; ul < pmdindex->IncludedCols(); ul++)
	{
		ULONG ulPos = pmdindex->IncludedColAt(ul);

		ULONG ulPosNonDropped = pmdrel->NonDroppedColAt(ulPos);
		GPOS_ASSERT(gpos::ulong_max != ulPosNonDropped);
		GPOS_ASSERT(ulPosNonDropped < colref_array->Size());
		CColRef *colref = (*colref_array)[ulPosNonDropped];
		pdrgpcrIndex->Append(colref);
	}

	return pdrgpcrIndex;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FIndexApplicable
//
//	@doc:
//		Check if an index is applicable given the required, output and scalar
//		expression columns
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FIndexApplicable(CMemoryPool *mp, const IMDIndex *pmdindex,
							  const IMDRelation *pmdrel,
							  CColRefArray *pdrgpcrOutput,
							  CColRefSet *pcrsScalar,
							  IMDIndex::EmdindexType emdindtype,
							  IMDIndex::EmdindexType altindtype)
{
	BOOL possible_ao_table = pmdrel->IsNonBlockTable() ||
							 pmdrel->RetrieveRelStorageType() ==
								 IMDRelation::ErelstorageMixedPartitioned;
	// GiST and Hash can match with either Btree or Bitmap indexes
	if (pmdindex->IndexType() == IMDIndex::EmdindGist ||
		pmdindex->IndexType() == IMDIndex::EmdindHash ||
		// GIN and BRIN can only match with Bitmap Indexes
		(emdindtype == IMDIndex::EmdindBitmap &&
		 (IMDIndex::EmdindGin == pmdindex->IndexType() ||
		  IMDIndex::EmdindBrin == pmdindex->IndexType())))
	{
		// continue
	}
	else if (emdindtype == IMDIndex::EmdindBitmap &&
			 pmdindex->IndexType() == IMDIndex::EmdindBtree &&
			 possible_ao_table)
	{
		// continue, Btree indexes on AO tables can be treated as Bitmap tables
	}
	else if (
		(emdindtype != pmdindex->IndexType() &&
		 altindtype !=
			 pmdindex
				 ->IndexType()) ||	// otherwise make sure the index matches the given type(s)
		0 == pcrsScalar->Size()	 // no columns to match index against
	)
	{
		return false;
	}

	BOOL fApplicable = true;

	CColRefSet *pcrsIndexCols =
		CXformUtils::PcrsIndexKeys(mp, pdrgpcrOutput, pmdindex, pmdrel);
	if (pcrsScalar->IsDisjoint(
			pcrsIndexCols))	 // indexing columns disjoint from the columns used in the scalar expression
	{
		fApplicable = false;
	}

	// clean up
	pcrsIndexCols->Release();

	return fApplicable;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprRowNumber
//
//	@doc:
//		Create an expression with "row_number" window function
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprRowNumber(CMemoryPool *mp)
{
	OID row_number_oid = COptCtxt::PoctxtFromTLS()
							 ->GetOptimizerConfig()
							 ->GetWindowOids()
							 ->OidRowNumber();

	CScalarWindowFunc *popRowNumber = GPOS_NEW(mp) CScalarWindowFunc(
		mp, GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidGeneral, row_number_oid),
		GPOS_NEW(mp) CMDIdGPDB(IMDId::EmdidGeneral, GPDB_INT8_OID),
		GPOS_NEW(mp) CWStringConst(mp, GPOS_WSZ_LIT("row_number")),
		CScalarWindowFunc::EwsImmediate, false /* is_distinct */,
		false /* is_star_arg */, false /* is_simple_agg */
	);

	CExpression *pexprScRowNumber = GPOS_NEW(mp) CExpression(mp, popRowNumber);

	return pexprScRowNumber;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprWindowWithRowNumber
//
//	@doc:
//		Create a sequence project (window) expression with a row_number
//		window function and partitioned by the given array of columns references
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprWindowWithRowNumber(CMemoryPool *mp,
									  CExpression *pexprWindowChild,
									  CColRefArray *pdrgpcrInput)
{
	// partitioning information
	CDistributionSpec *pds = nullptr;
	if (nullptr != pdrgpcrInput && 0 < pdrgpcrInput->Size())
	{
		CExpressionArray *pdrgpexprInput =
			CUtils::PdrgpexprScalarIdents(mp, pdrgpcrInput);
		pds = GPOS_NEW(mp) CDistributionSpecHashed(pdrgpexprInput,
												   true /* fNullsCollocated */);
	}
	else
	{
		pds = GPOS_NEW(mp)
			CDistributionSpecSingleton(CDistributionSpecSingleton::EstMaster);
	}

	// window frames
	CWindowFrameArray *pdrgpwf = GPOS_NEW(mp) CWindowFrameArray(mp);

	// ordering information
	COrderSpecArray *pdrgpos = GPOS_NEW(mp) COrderSpecArray(mp);

	// row_number window function project list
	CExpression *pexprScWindowFunc = PexprRowNumber(mp);

	// generate a new column reference
	CScalarWindowFunc *popScWindowFunc =
		CScalarWindowFunc::PopConvert(pexprScWindowFunc->Pop());
	const IMDType *pmdtype = COptCtxt::PoctxtFromTLS()->Pmda()->RetrieveType(
		popScWindowFunc->MdidType());
	CName name(popScWindowFunc->PstrFunc());
	CColRef *colref = COptCtxt::PoctxtFromTLS()->Pcf()->PcrCreate(
		pmdtype, popScWindowFunc->TypeModifier(), name);

	// new project element
	CScalarProjectElement *popScPrEl =
		GPOS_NEW(mp) CScalarProjectElement(mp, colref);

	// generate a project element
	CExpression *pexprProjElem =
		GPOS_NEW(mp) CExpression(mp, popScPrEl, pexprScWindowFunc);

	// generate the project list
	CExpression *pexprProjList = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarProjectList(mp), pexprProjElem);

	CLogicalSequenceProject *popLgSequence =
		GPOS_NEW(mp) CLogicalSequenceProject(
			mp, COperator::ESPType::EsptypeGlobalOneStep, pds, pdrgpos, pdrgpwf);

	pexprWindowChild->AddRef();
	CExpression *pexprLgSequence = GPOS_NEW(mp)
		CExpression(mp, popLgSequence, pexprWindowChild, pexprProjList);

	return pexprLgSequence;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprAssertOneRow
//
//	@doc:
//		Generate a logical Assert expression that errors out when more than
//		one row is generated
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprAssertOneRow(CMemoryPool *mp, CExpression *pexprChild)
{
	GPOS_ASSERT(nullptr != pexprChild);
	GPOS_ASSERT(pexprChild->Pop()->FLogical());

	CExpression *pexprSeqPrj =
		PexprWindowWithRowNumber(mp, pexprChild, nullptr /*pdrgpcrInput*/);
	CColRef *pcrRowNumber =
		CScalarProjectElement::PopConvert((*(*pexprSeqPrj)[1])[0]->Pop())
			->Pcr();
	CExpression *pexprCmp = CUtils::PexprScalarEqCmp(
		mp, pcrRowNumber, CUtils::PexprScalarConstInt4(mp, 1 /*value*/));

	CWStringConst *pstrErrorMsg = PstrErrorMessage(
		mp, gpos::CException::ExmaSQL, gpos::CException::ExmiSQLMaxOneRow);
	CExpression *pexprAssertConstraint = GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CScalarAssertConstraint(mp, pstrErrorMsg), pexprCmp);

	CExpression *pexprAssertPredicate = GPOS_NEW(mp)
		CExpression(mp, GPOS_NEW(mp) CScalarAssertConstraintList(mp),
					pexprAssertConstraint);

	CLogicalAssert *popAssert = GPOS_NEW(mp) CLogicalAssert(
		mp, GPOS_NEW(mp) CException(gpos::CException::ExmaSQL,
									gpos::CException::ExmiSQLMaxOneRow));

	return GPOS_NEW(mp)
		CExpression(mp, popAssert, pexprSeqPrj, pexprAssertPredicate);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PcrProjectElement
//
//	@doc:
//		Return the colref of the n-th project element
//---------------------------------------------------------------------------
CColRef *
CXformUtils::PcrProjectElement(CExpression *pexpr, ULONG ulIdxProjElement)
{
	CExpression *pexprProjList = (*pexpr)[1];
	GPOS_ASSERT(COperator::EopScalarProjectList ==
				pexprProjList->Pop()->Eopid());

	CExpression *pexprProjElement = (*pexprProjList)[ulIdxProjElement];
	GPOS_ASSERT(nullptr != pexprProjElement);

	return CScalarProjectElement::PopConvert(pexprProjElement->Pop())->Pcr();
}



// Lookup join keys in scalar child group
void
CXformUtils::LookupJoinKeys(CMemoryPool *mp, CExpression *pexpr,
							CExpressionArray **ppdrgpexprOuter,
							CExpressionArray **ppdrgpexprInner,
							IMdIdArray **join_opfamilies)
{
	GPOS_ASSERT(nullptr != ppdrgpexprOuter);
	GPOS_ASSERT(nullptr != ppdrgpexprInner);

	*ppdrgpexprOuter = nullptr;
	*ppdrgpexprInner = nullptr;
	*join_opfamilies = nullptr;

	CGroupExpression *pgexprScalarOrigin = (*pexpr)[2]->Pgexpr();
	if (nullptr == pgexprScalarOrigin)
	{
		return;
	}

	CColRefSet *pcrsOuterOutput = (*pexpr)[0]->DeriveOutputColumns();
	CColRefSet *pcrsInnerOutput = (*pexpr)[1]->DeriveOutputColumns();

	CGroup *pgroupScalar = pgexprScalarOrigin->Pgroup();
	if (nullptr == pgroupScalar->PdrgpexprJoinKeysOuter())
	{
		// hash join keys not found
		return;
	}

	GPOS_ASSERT(nullptr != pgroupScalar->PdrgpexprJoinKeysInner());

	if (IMdIdArray *opfamilies = pgroupScalar->JoinOpfamilies())
	{
		opfamilies->AddRef();
		*join_opfamilies = opfamilies;
	}

	// extract used columns by hash join keys
	CColRefSet *pcrsUsedOuter =
		CUtils::PcrsExtractColumns(mp, pgroupScalar->PdrgpexprJoinKeysOuter());
	CColRefSet *pcrsUsedInner =
		CUtils::PcrsExtractColumns(mp, pgroupScalar->PdrgpexprJoinKeysInner());

	BOOL fOuterKeysUsesOuterChild = pcrsOuterOutput->ContainsAll(pcrsUsedOuter);
	BOOL fInnerKeysUsesInnerChild = pcrsInnerOutput->ContainsAll(pcrsUsedInner);
	BOOL fInnerKeysUsesOuterChild = pcrsOuterOutput->ContainsAll(pcrsUsedInner);
	BOOL fOuterKeysUsesInnerChild = pcrsInnerOutput->ContainsAll(pcrsUsedOuter);

	if ((fOuterKeysUsesOuterChild && fInnerKeysUsesInnerChild) ||
		(fInnerKeysUsesOuterChild && fOuterKeysUsesInnerChild))
	{
		CGroupProxy gp(pgroupScalar);

		pgroupScalar->PdrgpexprJoinKeysOuter()->AddRef();
		pgroupScalar->PdrgpexprJoinKeysInner()->AddRef();

		// align hash join keys with join child
		if (fOuterKeysUsesOuterChild && fInnerKeysUsesInnerChild)
		{
			*ppdrgpexprOuter = pgroupScalar->PdrgpexprJoinKeysOuter();
			*ppdrgpexprInner = pgroupScalar->PdrgpexprJoinKeysInner();
		}
		else
		{
			GPOS_ASSERT(fInnerKeysUsesOuterChild && fOuterKeysUsesInnerChild);

			*ppdrgpexprOuter = pgroupScalar->PdrgpexprJoinKeysInner();
			*ppdrgpexprInner = pgroupScalar->PdrgpexprJoinKeysOuter();
		}
	}

	pcrsUsedOuter->Release();
	pcrsUsedInner->Release();
}


// Cache join keys on scalar child group
void
CXformUtils::CacheJoinKeys(CExpression *pexpr, CExpressionArray *pdrgpexprOuter,
						   CExpressionArray *pdrgpexprInner,
						   IMdIdArray *join_opfamilies)
{
	GPOS_ASSERT(nullptr != pdrgpexprOuter);
	GPOS_ASSERT(nullptr != pdrgpexprInner);

	CGroupExpression *pgexprScalarOrigin = (*pexpr)[2]->Pgexpr();
	if (nullptr != pgexprScalarOrigin)
	{
		CGroup *pgroupScalar = pgexprScalarOrigin->Pgroup();

		{  // scope of group proxy
			CGroupProxy gp(pgroupScalar);
			gp.SetJoinKeys(pdrgpexprOuter, pdrgpexprInner, join_opfamilies);
		}
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::AddCTEProducer
//
//	@doc:
//		Helper to create a CTE producer expression and add it to global
//		CTE info structure
//		Does not take ownership of pexpr
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprAddCTEProducer(CMemoryPool *mp, ULONG ulCTEId,
								 CColRefArray *colref_array, CExpression *pexpr)
{
	CColRefArray *pdrgpcrProd = CUtils::PdrgpcrCopy(mp, colref_array);
	UlongToColRefMap *colref_mapping =
		CUtils::PhmulcrMapping(mp, colref_array, pdrgpcrProd);
	CExpression *pexprRemapped = pexpr->PexprCopyWithRemappedColumns(
		mp, colref_mapping, true /*must_exist*/);
	colref_mapping->Release();

	CExpression *pexprProducer = GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CLogicalCTEProducer(mp, ulCTEId, pdrgpcrProd),
		pexprRemapped);

	CCTEInfo *pcteinfo = COptCtxt::PoctxtFromTLS()->Pcteinfo();
	pcteinfo->AddCTEProducer(pexprProducer);
	pexprProducer->Release();

	return pcteinfo->PexprCTEProducer(ulCTEId);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FProcessGPDBAntiSemiHashJoin
//
//	@doc:
//		Helper to extract equality from an expression tree of the form
//		OP
//		 |--(=)
//		 |	 |-- expr1
//		 |	 +-- expr2
//		 +--exprOther
//
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FExtractEquality(
	CExpression *pexpr,
	CExpression **
		ppexprEquality,	 // output: extracted equality expression, set to NULL if extraction failed
	CExpression **
		ppexprOther	 // output: sibling of equality expression, set to NULL if extraction failed
)
{
	GPOS_ASSERT(2 == pexpr->Arity());

	*ppexprEquality = nullptr;
	*ppexprOther = nullptr;

	CExpression *pexprLeft = (*pexpr)[0];
	CExpression *pexprRight = (*pexpr)[1];
	BOOL fEqualityOnLeft = CPredicateUtils::IsEqualityOp(pexprLeft);
	BOOL fEqualityOnRight = CPredicateUtils::IsEqualityOp(pexprRight);
	if (fEqualityOnLeft || fEqualityOnRight)
	{
		*ppexprEquality = pexprLeft;
		*ppexprOther = pexprRight;
		if (fEqualityOnRight)
		{
			*ppexprEquality = pexprRight;
			*ppexprOther = pexprLeft;
		}

		return true;
	}

	return false;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FProcessGPDBAntiSemiHashJoin
//
//	@doc:
//		GPDB hash join return no results if the inner side of anti-semi-join
//		produces null values, this allows simplifying join predicates of the
//		form (equality_expr IS DISTINCT FROM false) to (equality_expr) since
//		GPDB hash join operator guarantees no join results to be returned in
//		this case
//
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FProcessGPDBAntiSemiHashJoin(
	CMemoryPool *mp, CExpression *pexpr,
	CExpression **
		ppexprResult  // output: result expression, set to NULL if processing failed
)
{
	GPOS_ASSERT(nullptr != ppexprResult);
	GPOS_ASSERT(
		COperator::EopLogicalLeftAntiSemiJoin == pexpr->Pop()->Eopid() ||
		COperator::EopLogicalLeftAntiSemiJoinNotIn == pexpr->Pop()->Eopid());

	*ppexprResult = nullptr;
	CExpression *pexprOuter = (*pexpr)[0];
	CExpression *pexprInner = (*pexpr)[1];
	CExpression *pexprScalar = (*pexpr)[2];

	CExpressionArray *pdrgpexpr =
		CPredicateUtils::PdrgpexprConjuncts(mp, pexprScalar);
	CExpressionArray *pdrgpexprNew = GPOS_NEW(mp) CExpressionArray(mp);
	const ULONG ulPreds = pdrgpexpr->Size();
	BOOL fSimplifiedPredicate = false;
	for (ULONG ul = 0; ul < ulPreds; ul++)
	{
		CExpression *pexprPred = (*pdrgpexpr)[ul];
		if (CPredicateUtils::FIDFFalse(pexprPred))
		{
			CExpression *pexprEquality = nullptr;
			CExpression *pexprFalse = nullptr;
			if (FExtractEquality(
					pexprPred, &pexprEquality,
					&pexprFalse) &&	 // extracted equality expression
				IMDId::EmdidGeneral ==
					CScalarConst::PopConvert(pexprFalse->Pop())
						->GetDatum()
						->MDId()
						->MdidType() &&	 // underlying system is GPDB
				CPhysicalJoin::FHashJoinCompatible(
					pexprEquality, pexprOuter,
					pexprInner) &&	// equality is hash-join compatible
				!CUtils::FUsesNullableCol(
					mp, pexprEquality,
					pexprInner) &&	// equality uses an inner NOT NULL column
				!CUtils::FUsesNullableCol(
					mp, pexprEquality,
					pexprOuter))  // equality uses an outer NOT NULL column
			{
				pexprEquality->AddRef();
				pdrgpexprNew->Append(pexprEquality);
				fSimplifiedPredicate = true;
				continue;
			}
		}
		pexprPred->AddRef();
		pdrgpexprNew->Append(pexprPred);
	}

	pdrgpexpr->Release();
	if (!fSimplifiedPredicate)
	{
		pdrgpexprNew->Release();
		return false;
	}

	pexprOuter->AddRef();
	pexprInner->AddRef();
	pexpr->Pop()->AddRef();
	*ppexprResult = GPOS_NEW(mp)
		CExpression(mp, pexpr->Pop(), pexprOuter, pexprInner,
					CPredicateUtils::PexprConjunction(mp, pdrgpexprNew));

	return true;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprBuildIndexPlan
//
//	@doc:
//		Construct an expression representing a new access path using the given functors for
//		operator constructors and rewritten access path.
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprBuildBtreeIndexPlan(CMemoryPool *mp, CMDAccessor *md_accessor,
									  CExpression *pexprGet, ULONG ulOriginOpId,
									  CExpressionArray *pdrgpexprConds,
									  CColRefSet *pcrsScalarExpr,
									  CColRefSet *outer_refs,
									  const IMDIndex *pmdindex,
									  const IMDRelation *pmdrel,
									  EIndexScanDirection indexscanDirection,
									  BOOL indexForOrderBy, BOOL indexonly)
{
	GPOS_ASSERT(nullptr != pexprGet);
	GPOS_ASSERT(nullptr != pdrgpexprConds);
	GPOS_ASSERT(nullptr != pcrsScalarExpr);
	GPOS_ASSERT(nullptr != pmdindex);
	GPOS_ASSERT(nullptr != pmdrel);

	COperator::EOperatorId op_id = pexprGet->Pop()->Eopid();
	GPOS_ASSERT(CLogical::EopLogicalGet == op_id ||
				CLogical::EopLogicalDynamicGet == op_id);

	BOOL fDynamicGet = (COperator::EopLogicalDynamicGet == op_id);

	CTableDescriptorHashSet *ptabdescset = pexprGet->DeriveTableDescriptor();
	GPOS_ASSERT(1 == ptabdescset->Size());
	CTableDescriptor *ptabdesc = ptabdescset->First();

	CColRefArray *pdrgpcrOutput = nullptr;
	CWStringConst *alias = nullptr;
	ULONG ulPartIndex = gpos::ulong_max;
	CColRef2dArray *pdrgpdrgpcrPart = nullptr;
	IMdIdArray *partition_mdids = nullptr;

	if (ptabdesc->RetrieveRelStorageType() != IMDRelation::ErelstorageHeap &&
		(pmdindex->IndexType() == IMDIndex::EmdindGist ||
		 pmdindex->IndexType() == IMDIndex::EmdindHash))
	{
		// Non-heap tables not supported for GiST
		return nullptr;
	}

	if (fDynamicGet)
	{
		CLogicalDynamicGet *popDynamicGet =
			CLogicalDynamicGet::PopConvert(pexprGet->Pop());

		ulPartIndex = popDynamicGet->ScanId();
		pdrgpcrOutput = popDynamicGet->PdrgpcrOutput();
		GPOS_ASSERT(nullptr != pdrgpcrOutput);
		alias = GPOS_NEW(mp)
			CWStringConst(mp, popDynamicGet->Name().Pstr()->GetBuffer());
		pdrgpdrgpcrPart = popDynamicGet->PdrgpdrgpcrPart();
		partition_mdids = popDynamicGet->GetPartitionMdids();
	}
	else
	{
		CLogicalGet *popGet = CLogicalGet::PopConvert(pexprGet->Pop());
		pdrgpcrOutput = popGet->PdrgpcrOutput();
		GPOS_ASSERT(nullptr != pdrgpcrOutput);
		alias =
			GPOS_NEW(mp) CWStringConst(mp, popGet->Name().Pstr()->GetBuffer());
	}

	if (!FIndexApplicable(mp, pmdindex, pmdrel, pdrgpcrOutput, pcrsScalarExpr,
						  IMDIndex::EmdindBtree))
	{
		GPOS_DELETE(alias);

		return nullptr;
	}

	CColRefArray *pdrgppcrIndexCols =
		PdrgpcrIndexKeys(mp, pdrgpcrOutput, pmdindex, pmdrel);
	CExpressionArray *pdrgpexprIndex = GPOS_NEW(mp) CExpressionArray(mp);

	// pdrgpexprResidual holds all the predicate conditions that can't leverage
	// indexing (either don't refer to index keys at all, or,
	// refer to index keys but can't make use of indexing)
	CExpressionArray *pdrgpexprResidual = GPOS_NEW(mp) CExpressionArray(mp);

	// Count of predicates which don't refer to index keys at all.
	// This, count is used in costing index scans.
	// Eg: Table : foo (a,b,c,d); Index keys: a,b; Predicate on: a,c,d; here:
	// UnindexedPredCol =c,d and ulUnindexedPredColCount - 2
	ULONG ulUnindexedPredColCount = 0;

	CPredicateUtils::ExtractIndexPredicates(
		mp, md_accessor, pdrgpexprConds, pmdindex, pdrgppcrIndexCols,
		pdrgpexprIndex, pdrgpexprResidual, ulUnindexedPredColCount, outer_refs);
	CColRefSet *outer_refs_in_index_get =
		CUtils::PcrsExtractColumns(mp, pdrgpexprIndex);
	outer_refs_in_index_get->Intersection(outer_refs);

	// exit early if:
	// (1) there are no index-able predicates or
	// (2) there are no outer references in index-able predicates
	//
	// (2) is valid only for Join2IndexApply xform wherein the index-get
	// expression must include outer references for it to be an alternative
	// worth considering. Otherwise, it has the same effect as a regular NLJ
	// with an index lookup.
	//
	// Both (1) and (2) doesn't apply if index is used for ORDER BY. Because
	// a query with just order by doesn't have index-able predicates.
	if ((0 == pdrgpexprIndex->Size() || outer_refs_in_index_get->Size() == 0) &&
		!indexForOrderBy)
	{
		// clean up
		GPOS_DELETE(alias);
		pdrgppcrIndexCols->Release();
		pdrgpexprResidual->Release();
		pdrgpexprIndex->Release();
		outer_refs_in_index_get->Release();

		return nullptr;
	}

	// most GiST indexes are lossy, so conservatively re-add all the index quals to the residual so that they can be rechecked
	if (pmdindex->IndexType() == IMDIndex::EmdindGist)
	{
		for (ULONG ul = 0; ul < pdrgpexprIndex->Size(); ul++)
		{
			CExpression *pexprPred = (*pdrgpexprIndex)[ul];
			pexprPred->AddRef();
			pdrgpexprResidual->Append(pexprPred);
		}
	}
	else
	{
		GPOS_ASSERT(pdrgpexprConds->Size() ==
					pdrgpexprResidual->Size() + pdrgpexprIndex->Size());
	}

	ptabdesc->AddRef();
	pdrgpcrOutput->AddRef();
	// create the logical (dynamic) bitmap table get operator
	CLogical *popLogicalGet = nullptr;

	if (fDynamicGet)
	{
		pdrgpdrgpcrPart->AddRef();
		partition_mdids->AddRef();
		if (indexonly)
		{
			popLogicalGet =
				PopDynamicBtreeIndexOpConstructor<CLogicalDynamicIndexOnlyGet>(
					mp, pmdindex, ptabdesc, ulOriginOpId,
					GPOS_NEW(mp) CName(mp, CName(alias)), ulPartIndex,
					pdrgpcrOutput, pdrgpdrgpcrPart, partition_mdids,
					ulUnindexedPredColCount);
			if (!CHintUtils::SatisfiesPlanHints(
					CLogicalDynamicIndexOnlyGet::PopConvert(popLogicalGet),
					COptCtxt::PoctxtFromTLS()
						->GetOptimizerConfig()
						->GetPlanHint()))
			{
				// clean up
				GPOS_DELETE(alias);
				pdrgppcrIndexCols->Release();
				pdrgpexprResidual->Release();
				pdrgpexprIndex->Release();
				outer_refs_in_index_get->Release();
				popLogicalGet->Release();
				return nullptr;
			}
		}
		else
		{
			popLogicalGet =
				PopDynamicBtreeIndexOpConstructor<CLogicalDynamicIndexGet>(
					mp, pmdindex, ptabdesc, ulOriginOpId,
					GPOS_NEW(mp) CName(mp, CName(alias)), ulPartIndex,
					pdrgpcrOutput, pdrgpdrgpcrPart, partition_mdids,
					ulUnindexedPredColCount);
			if (!CHintUtils::SatisfiesPlanHints(
					CLogicalDynamicIndexGet::PopConvert(popLogicalGet),
					COptCtxt::PoctxtFromTLS()
						->GetOptimizerConfig()
						->GetPlanHint()))
			{
				// clean up
				GPOS_DELETE(alias);
				pdrgppcrIndexCols->Release();
				pdrgpexprResidual->Release();
				pdrgpexprIndex->Release();
				outer_refs_in_index_get->Release();
				popLogicalGet->Release();
				return nullptr;
			}
		}
	}
	else
	{
		if (indexonly)
		{
			popLogicalGet =
				PopStaticBtreeIndexOpConstructor<CLogicalIndexOnlyGet>(
					mp, pmdindex, ptabdesc, ulOriginOpId,
					GPOS_NEW(mp) CName(mp, CName(alias)), pdrgpcrOutput,
					ulUnindexedPredColCount, indexscanDirection);
			if (!CHintUtils::SatisfiesPlanHints(
					CLogicalIndexOnlyGet::PopConvert(popLogicalGet),
					COptCtxt::PoctxtFromTLS()
						->GetOptimizerConfig()
						->GetPlanHint()))
			{
				// clean up
				GPOS_DELETE(alias);
				pdrgppcrIndexCols->Release();
				pdrgpexprResidual->Release();
				pdrgpexprIndex->Release();
				outer_refs_in_index_get->Release();
				popLogicalGet->Release();
				return nullptr;
			}
		}
		else
		{
			popLogicalGet = PopStaticBtreeIndexOpConstructor<CLogicalIndexGet>(
				mp, pmdindex, ptabdesc, ulOriginOpId,
				GPOS_NEW(mp) CName(mp, CName(alias)), pdrgpcrOutput,
				ulUnindexedPredColCount, indexscanDirection);
			if (!CHintUtils::SatisfiesPlanHints(
					CLogicalIndexGet::PopConvert(popLogicalGet),
					COptCtxt::PoctxtFromTLS()
						->GetOptimizerConfig()
						->GetPlanHint()))
			{
				// clean up
				GPOS_DELETE(alias);
				pdrgppcrIndexCols->Release();
				pdrgpexprResidual->Release();
				pdrgpexprIndex->Release();
				outer_refs_in_index_get->Release();
				popLogicalGet->Release();
				return nullptr;
			}
		}
	}

	// clean up
	GPOS_DELETE(alias);
	pdrgppcrIndexCols->Release();
	outer_refs_in_index_get->Release();

	CExpression *pexprIndexCond =
		CPredicateUtils::PexprConjunction(mp, pdrgpexprIndex);
	CExpression *pexprResidualCond =
		CPredicateUtils::PexprConjunction(mp, pdrgpexprResidual);

	return PexprRewrittenBtreeIndexPath(mp, pexprIndexCond, pexprResidualCond,
										pmdindex, ptabdesc, popLogicalGet);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprScalarBitmapBoolOp
//
//	@doc:
//		 Helper for creating BitmapBoolOp expression
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprScalarBitmapBoolOp(
	CMemoryPool *mp, CMDAccessor *md_accessor, CExpression *pexprOriginalPred,
	CExpressionArray *pdrgpexpr, CTableDescriptor *ptabdesc,
	const IMDRelation *pmdrel, CColRefArray *pdrgpcrOutput,
	CColRefSet *outer_refs, CColRefSet *pcrsReqd, BOOL fConjunction,
	CExpression **ppexprRecheck, CExpression **ppexprResidual,
	BOOL isAPartialPredicate)
{
	GPOS_ASSERT(nullptr != pdrgpexpr);

	const ULONG ulPredicates = pdrgpexpr->Size();

	// array of recheck predicates
	CExpressionArray *pdrgpexprRecheckNew = GPOS_NEW(mp) CExpressionArray(mp);

	// array of residual predicates
	CExpressionArray *pdrgpexprResidualNew = GPOS_NEW(mp) CExpressionArray(mp);

	// array of bitmap index probe/bitmap bool op expressions
	CExpressionArray *pdrgpexprBitmap = GPOS_NEW(mp) CExpressionArray(mp);

	CreateBitmapIndexProbeOps(
		mp, md_accessor, pexprOriginalPred, pdrgpexpr, ptabdesc, pmdrel,
		pdrgpcrOutput, outer_refs, pcrsReqd, fConjunction, pdrgpexprBitmap,
		pdrgpexprRecheckNew, pdrgpexprResidualNew, isAPartialPredicate);

	GPOS_ASSERT(pdrgpexprRecheckNew->Size() == pdrgpexprBitmap->Size());

	const ULONG ulBitmapExpr = pdrgpexprBitmap->Size();

	if (0 == ulBitmapExpr || (!fConjunction && ulBitmapExpr < ulPredicates))
	{
		// no relevant bitmap indexes found,
		// or expression is a disjunction and some disjuncts don't have applicable bitmap indexes
		pdrgpexprBitmap->Release();
		pdrgpexprRecheckNew->Release();
		pdrgpexprResidualNew->Release();
		return nullptr;
	}

	CExpression *pexprBitmapBoolOp = nullptr;
	CExpression *pexprRecheckNew = nullptr;

	JoinBitmapIndexProbes(mp, pdrgpexprBitmap, pdrgpexprRecheckNew,
						  fConjunction, &pexprBitmapBoolOp, &pexprRecheckNew);

	if (nullptr != *ppexprRecheck)
	{
		CExpression *pexprRecheckNewCombined = CPredicateUtils::PexprConjDisj(
			mp, *ppexprRecheck, pexprRecheckNew, fConjunction);
		(*ppexprRecheck)->Release();
		pexprRecheckNew->Release();
		*ppexprRecheck = pexprRecheckNewCombined;
	}
	else
	{
		*ppexprRecheck = pexprRecheckNew;
	}

	if (0 < pdrgpexprResidualNew->Size())
	{
		ComputeBitmapTableScanResidualPredicate(
			mp, fConjunction, pexprOriginalPred, ppexprResidual,
			pdrgpexprResidualNew);
	}

	// cleanup
	pdrgpexprRecheckNew->Release();
	pdrgpexprBitmap->Release();
	pdrgpexprResidualNew->Release();

	return pexprBitmapBoolOp;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::ComputeBitmapTableScanResidualPredicate
//
//	@doc:
//		Compute the residual predicate for a bitmap table scan
//
//---------------------------------------------------------------------------
void
CXformUtils::ComputeBitmapTableScanResidualPredicate(
	CMemoryPool *mp, BOOL fConjunction, CExpression *pexprOriginalPred,
	CExpression **
		ppexprResidual,	 // input-output argument: the residual predicate computed so-far, and resulting predicate
	CExpressionArray *pdrgpexprResidualNew)
{
	GPOS_ASSERT(nullptr != pexprOriginalPred);
	GPOS_ASSERT(0 < pdrgpexprResidualNew->Size());

	if (!fConjunction)
	{
		// one of the disjuncts requires a residual predicate: we need to reevaluate the original predicate
		// for example, for index keys ik1 and ik2, the following will require re-evaluating
		// the whole predicate rather than just k < 100:
		// ik1 = 1 or (ik2=2 and k<100)
		pexprOriginalPred->AddRef();
		CRefCount::SafeRelease(*ppexprResidual);
		*ppexprResidual = pexprOriginalPred;
		return;
	}

	pdrgpexprResidualNew->AddRef();
	CExpression *pexprResidualNew =
		CPredicateUtils::PexprConjDisj(mp, pdrgpexprResidualNew, fConjunction);

	if (nullptr != *ppexprResidual)
	{
		CExpression *pexprResidualNewCombined = CPredicateUtils::PexprConjDisj(
			mp, *ppexprResidual, pexprResidualNew, fConjunction);
		(*ppexprResidual)->Release();
		pexprResidualNew->Release();
		*ppexprResidual = pexprResidualNewCombined;
	}
	else
	{
		*ppexprResidual = pexprResidualNew;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprBitmapBoolOp
//
//	@doc:
//		Construct a bitmap bool op expression between the given bitmap access
// 		path expressions
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprBitmapBoolOp(CMemoryPool *mp, IMDId *pmdidBitmapType,
							   CExpression *pexprLeft, CExpression *pexprRight,
							   BOOL fConjunction)
{
	GPOS_ASSERT(nullptr != pexprLeft);
	GPOS_ASSERT(nullptr != pexprRight);

	CScalarBitmapBoolOp::EBitmapBoolOp ebitmapboolop =
		CScalarBitmapBoolOp::EbitmapboolAnd;

	if (!fConjunction)
	{
		ebitmapboolop = CScalarBitmapBoolOp::EbitmapboolOr;
	}

	return GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CScalarBitmapBoolOp(mp, ebitmapboolop, pmdidBitmapType),
		pexprLeft, pexprRight);
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprConditionOnBoolColumn
//
//	@doc:
//		Construct a bitmap index path expression for the given predicate
//		out of the children of the given expression.
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprBitmapLookupWithPredicateBreakDown(
	CMemoryPool *mp, CMDAccessor *md_accessor, CExpression *pexprOriginalPred,
	CExpression *pexprPred, CTableDescriptor *ptabdesc,
	const IMDRelation *pmdrel, CColRefArray *pdrgpcrOutput,
	CColRefSet *outer_refs, CColRefSet *pcrsReqd, CExpression **ppexprRecheck,
	CExpression **ppexprResidual)
{
	GPOS_ASSERT(nullptr == *ppexprRecheck);
	GPOS_ASSERT(nullptr == *ppexprResidual);

	CExpressionArray *pdrgpexpr = nullptr;
	BOOL fConjunction = CPredicateUtils::FAnd(pexprPred);

	if (fConjunction)
	{
		// Combine all the supported conjuncts into a single CExpression, to be able to
		// pass them as one unit to PexprScalarBitmapBoolOp and to PexprBitmapSelectBestIndex,
		// since we have optimizations that find the best multi-column index.

		CExpressionArray *temp_conjuncts =
			CPredicateUtils::PdrgpexprConjuncts(mp, pexprPred);
		CExpressionArray *supported_conjuncts =
			GPOS_NEW(mp) CExpressionArray(mp);

		pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);

		const ULONG size = temp_conjuncts->Size();
		for (ULONG ul = 0; ul < size; ul++)
		{
			CExpression *pexpr = (*temp_conjuncts)[ul];

			pexpr->AddRef();
			if (CPredicateUtils::FBitmapLookupSupportedPredicateOrConjunct(
					pexpr, outer_refs))
			{
				supported_conjuncts->Append(pexpr);
			}
			else
			{
				pdrgpexpr->Append(pexpr);
			}
		}
		temp_conjuncts->Release();

		if (0 < supported_conjuncts->Size())
		{
			CExpression *anded_expr =
				CPredicateUtils::PexprConjunction(mp, supported_conjuncts);
			pdrgpexpr->Append(anded_expr);
		}
		else
		{
			supported_conjuncts->Release();
		}
	}
	else
	{
		pdrgpexpr = CPredicateUtils::PdrgpexprDisjuncts(mp, pexprPred);
	}

	if (1 == pdrgpexpr->Size())
	{
		// unsupported predicate that cannot be split further into conjunctions and disjunctions
		pdrgpexpr->Release();
		return nullptr;
	}

	// expression is a deeper tree: recurse further in each of the components
	CExpression *pexprResult = PexprScalarBitmapBoolOp(
		mp, md_accessor, pexprOriginalPred, pdrgpexpr, ptabdesc, pmdrel,
		pdrgpcrOutput, outer_refs, pcrsReqd, fConjunction, ppexprRecheck,
		ppexprResidual,
		!fConjunction /* we are now breaking up something other than an AND
												 predicate and want to consider BTree indexes as well */
	);
	pdrgpexpr->Release();

	return pexprResult;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprBitmapSelectBestIndex
//
//	@doc:
//		Given conjuncts of supported predicates, select the best index and
//		construct a bitmap index path expression. Return unused predicates
//		as residuals.
//		Examples for conjuncts of supported predicates:
//		  col op <const>
//		  col op <outer ref>
//		  col op <const> AND col op <outer ref>
//		Casts are also acceptable here.
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprBitmapSelectBestIndex(
	CMemoryPool *mp, CMDAccessor *md_accessor, CExpression *pexprPred,
	CTableDescriptor *ptabdesc, const IMDRelation *pmdrel,
	CColRefArray *pdrgpcrOutput, CColRefSet *pcrsOuterRefs,
	CExpression **ppexprRecheck, CExpression **ppexprResidual,
	BOOL considerBitmapAltForArrayCmp)
{
	CColRefSet *pcrsScalar = pexprPred->DeriveUsedColumns();
	ULONG ulBestIndex = 0;
	CExpression *pexprIndexFinal = nullptr;
	CDouble bestSelectivity =
		CDouble(2.0);  // selectivity can be a max value of 1
	ULONG bestNumResiduals = gpos::ulong_max;
	ULONG bestNumIndexCols = gpos::ulong_max;
	IMDIndex::EmdindexType altIndexType = IMDIndex::EmdindBitmap;

	if (considerBitmapAltForArrayCmp)
	{
		altIndexType = IMDIndex::EmdindBtree;
	}

	const ULONG ulIndexes = pmdrel->IndexCount();
	ULONG ulUnsupportedPredCount = 0;

	for (ULONG ul = 0; ul < ulIndexes; ul++)
	{
		const IMDIndex *pmdindex =
			md_accessor->RetrieveIndex(pmdrel->IndexMDidAt(ul));

		if (CXformUtils::FIndexApplicable(mp, pmdindex, pmdrel, pdrgpcrOutput,
										  pcrsScalar, IMDIndex::EmdindBitmap,
										  altIndexType))
		{
			// found an applicable index
			CExpressionArray *pdrgpexprScalar =
				CPredicateUtils::PdrgpexprConjuncts(mp, pexprPred);
			CColRefArray *pdrgpcrIndexCols =
				PdrgpcrIndexKeys(mp, pdrgpcrOutput, pmdindex, pmdrel);
			CExpressionArray *pdrgpexprIndex =
				GPOS_NEW(mp) CExpressionArray(mp);
			CExpressionArray *pdrgpexprResidual =
				GPOS_NEW(mp) CExpressionArray(mp);

			CPredicateUtils::ExtractIndexPredicates(
				mp, md_accessor, pdrgpexprScalar, pmdindex, pdrgpcrIndexCols,
				pdrgpexprIndex, pdrgpexprResidual, ulUnsupportedPredCount,
				pcrsOuterRefs, considerBitmapAltForArrayCmp);

			pdrgpexprScalar->Release();

			if (0 == pdrgpexprIndex->Size())
			{
				// no usable predicate, clean up
				pdrgpcrIndexCols->Release();
				pdrgpexprIndex->Release();
				pdrgpexprResidual->Release();
				continue;
			}

			BOOL fCompatible = CPredicateUtils::FCompatiblePredicates(
				pdrgpexprIndex, pmdindex, pdrgpcrIndexCols, md_accessor);
			pdrgpcrIndexCols->Release();

			if (!fCompatible)
			{
				pdrgpexprIndex->Release();
				pdrgpexprResidual->Release();
				continue;
			}

			pdrgpexprIndex->AddRef();
			CExpression *pexprIndex =
				CPredicateUtils::PexprConjunction(mp, pdrgpexprIndex);

			CDouble selectivity = CFilterStatsProcessor::SelectivityOfPredicate(
				mp, pexprIndex, ptabdesc, pcrsOuterRefs);

			pexprIndex->Release();

			BOOL possible_ao_table =
				pmdrel->IsNonBlockTable() ||
				pmdrel->RetrieveRelStorageType() ==
					IMDRelation::ErelstorageMixedPartitioned;

			// Btree indexes on AO tables are only great when the NDV is high. Do this check here
			if (selectivity > AO_TABLE_BTREE_INDEX_SELECTIVITY_THRESHOLD &&
				possible_ao_table &&
				pmdindex->IndexType() == IMDIndex::EmdindBtree)
			{
				pdrgpexprIndex->Release();
				pdrgpexprResidual->Release();
				continue;
			}

			CColRefArray *indexColumns = CXformUtils::PdrgpcrIndexKeys(
				mp, pdrgpcrOutput, pmdindex, pmdrel);

			// make sure the first key of index is included in the scalar predicate
			// (except for BRIN, which are symmetrical)
			// FIXME: Consider removing the first column check for GIN as well
			const CColRef *pcrFirstIndexKey = (*indexColumns)[0];

			if (!pcrsScalar->FMember(pcrFirstIndexKey) &&
				pmdindex->IndexType() != IMDIndex::EmdindBrin)
			{
				indexColumns->Release();
				pdrgpexprIndex->Release();
				pdrgpexprResidual->Release();
				continue;
			}

			ULONG numResiduals = pdrgpexprResidual->Size();
			ULONG numIndexCols = indexColumns->Size();
			// Score indexes by using three criteria:
			// - selectivity of the index predicate (more selective, i.e. smaller selectivity value, is better)
			// - number of residual predicates (fewer is better)
			// - number of columns in the index
			//   (with the same selectivity and # of residual preds, a smaller index is better)
			if (bestSelectivity > selectivity ||
				(bestSelectivity == selectivity &&
				 (bestNumResiduals > numResiduals ||
				  (bestNumResiduals == numResiduals &&
				   bestNumIndexCols > numIndexCols))))
			{
				CRefCount::SafeRelease((*ppexprResidual));
				pdrgpexprResidual->AddRef();
				(*ppexprResidual) = CPredicateUtils::PexprConjDisj(
					mp, pdrgpexprResidual, true /* fConjunction */);

				// if the index covers all the columns in the predicate, the residual generated is a trivial
				// constant true filter. Stop the search as this is an optimal index and discard the residual.
				if (CUtils::FScalarConstTrue((*ppexprResidual)))
				{
					(*ppexprResidual)->Release();
					(*ppexprResidual) = nullptr;
				}

				ulBestIndex = ul;
				bestSelectivity = selectivity;
				bestNumResiduals = numResiduals;
				bestNumIndexCols = numIndexCols;
				pdrgpexprIndex->AddRef();
				CRefCount::SafeRelease(pexprIndexFinal);
				pexprIndexFinal =
					CPredicateUtils::PexprConjunction(mp, pdrgpexprIndex);
			}

			pdrgpexprIndex->Release();
			pdrgpexprResidual->Release();
			indexColumns->Release();
		}
	}

	// if the final best index was found, return the correct expression
	if (nullptr != pexprIndexFinal)
	{
		const IMDIndex *pmdindex =
			md_accessor->RetrieveIndex(pmdrel->IndexMDidAt(ulBestIndex));
		CIndexDescriptor *pindexdesc =
			CIndexDescriptor::Pindexdesc(mp, ptabdesc, pmdindex);
		pmdindex->GetIndexRetItemTypeMdid()->AddRef();
		pexprIndexFinal->AddRef();
		(*ppexprRecheck) = pexprIndexFinal;

		ptabdesc->AddRef();
		CScalarBitmapIndexProbe *pop = GPOS_NEW(mp) CScalarBitmapIndexProbe(
			mp, pindexdesc, ptabdesc, pmdindex->GetIndexRetItemTypeMdid());
		if (!CHintUtils::SatisfiesPlanHints(
				pop,
				COptCtxt::PoctxtFromTLS()->GetOptimizerConfig()->GetPlanHint()))
		{
			pop->Release();
			pexprIndexFinal->Release();
			(*ppexprRecheck)->Release();
			return nullptr;
		}
		return GPOS_NEW(mp) CExpression(mp, pop, pexprIndexFinal);
	}

	// else the unmatched predicate becomes the residual
	pexprPred->AddRef();
	(*ppexprResidual) = pexprPred;
	return nullptr;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::CreateBitmapIndexProbeOps
//
//	@doc:
//		Given an array of predicate expressions, construct a bitmap access path
//		expression for each predicate and accumulate it in the pdrgpexprBitmap array
//
//---------------------------------------------------------------------------
void
CXformUtils::CreateBitmapIndexProbeOps(
	CMemoryPool *mp, CMDAccessor *md_accessor, CExpression *pexprOriginalPred,
	CExpressionArray *pdrgpexprPreds, CTableDescriptor *ptabdesc,
	const IMDRelation *pmdrel, CColRefArray *pdrgpcrOutput,
	CColRefSet *outer_refs, CColRefSet *pcrsReqd,
	BOOL,  // fConjunction
	CExpressionArray *pdrgpexprBitmap, CExpressionArray *pdrgpexprRecheck,
	CExpressionArray *pdrgpexprResidual, BOOL isAPartialPredicate)
{
	GPOS_ASSERT(nullptr != pdrgpexprPreds);

	ULONG ulPredicates = pdrgpexprPreds->Size();

	for (ULONG ul = 0; ul < ulPredicates; ul++)
	{
		CExpression *pexprPred = (*pdrgpexprPreds)[ul];
		CExpression *pexprBitmap = nullptr;
		CExpression *pexprRecheck = nullptr;

		CreateBitmapIndexProbesWithOrWithoutPredBreakdown(
			mp, md_accessor, pexprOriginalPred, pexprPred, ptabdesc, pmdrel,
			pdrgpcrOutput, outer_refs, pcrsReqd, &pexprBitmap, &pexprRecheck,
			pdrgpexprResidual, isAPartialPredicate);

		if (nullptr != pexprBitmap)
		{
			pdrgpexprBitmap->Append(pexprBitmap);
			pdrgpexprRecheck->Append(pexprRecheck);
		}
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::CreateBitmapIndexProbesWithOrWithoutPredBreakdown
//
//	@doc:
//		Given a predicate pexprPred, try one of two strategies, in a loop:
//			1. if it is a supported predicate, call PexprBitmapSelectBestIndex
//			2. else, call PexprBitmapLookupWithPredicateBreakDown to find indexes
//				for parts of the predicate
//
//		This method also has one optimization:
//		- try using multiple indexes by redriving the logic with the
//		  residual predicates, finding a second-best index
//
//---------------------------------------------------------------------------
void
CXformUtils::CreateBitmapIndexProbesWithOrWithoutPredBreakdown(
	CMemoryPool *pmp, CMDAccessor *pmda, CExpression *pexprOriginalPred,
	CExpression *pexprPred, CTableDescriptor *ptabdesc,
	const IMDRelation *pmdrel, CColRefArray *pdrgpcrOutput,
	CColRefSet *pcrsOuterRefs, CColRefSet *pcrsReqd,
	CExpression **pexprBitmapResult, CExpression **pexprRecheckResult,
	CExpressionArray *pdrgpexprResidualResult, BOOL isAPartialPredicate)
{
	CExpression *pexprRecheckLocal, *pexprResidualLocal, *pexprBitmapLocal;

	BOOL retryIndexLookupWithResidual = false;

	// create temporary arrays in which we accumulate indexes and preds
	// when we try multiple indexes, these are always ANDed together
	CExpressionArray *pdrgpexprBitmapTemp = GPOS_NEW(pmp) CExpressionArray(pmp);
	CExpressionArray *pdrgpexprRecheckTemp =
		GPOS_NEW(pmp) CExpressionArray(pmp);

	pexprPred->AddRef();

	while (nullptr != pexprPred)
	{
		pexprRecheckLocal = pexprResidualLocal = pexprBitmapLocal = nullptr;

		if (CPredicateUtils::FBitmapLookupSupportedPredicateOrConjunct(
				pexprPred, pcrsOuterRefs))
		{
			// do not break the predicate down and lookup for an index covering maximum predicate columns,
			// this is done in following scenario to generate optimal index paths.
			// predicate is a conjunct tree with children of the form :
			// "ident op const" or "ident or const-array" or boolean-ident or "NOT boolean-ident",
			// where the casts are allowed on both idents and constants.

			// example, with schema: t(a, b, c , d , e , f , g , h)
			// indexes: i1 (b, c, d), i2 (g, h) and i3(d)
			// for predicate: d = 1 AND b = 2 AND c = 3 AND e = 4 AND g = 5 AND h = 6
			// the index paths chosen will be:
			// i1 covering (d = 1 AND b = 2 AND c = 3) and i2 covering (g = 5 AND h = 6)
			// with residual as e = 4.

			BOOL isAPartialPredicateOrArrayCmp = isAPartialPredicate;

			if (!isAPartialPredicateOrArrayCmp)
			{
				// consider a bitmap index scan on a btree index if we find any array comparisons,
				// since we currently don't support those for regular index scans
				CExpressionArray *conjuncts =
					CPredicateUtils::PdrgpexprConjuncts(pmp, pexprPred);
				ULONG size = conjuncts->Size();

				for (ULONG i = 0; i < size && !isAPartialPredicateOrArrayCmp;
					 i++)
				{
					isAPartialPredicateOrArrayCmp =
						CPredicateUtils::FArrayCompareIdentToConstIgnoreCast(
							(*conjuncts)[i]);
				}

				conjuncts->Release();
			}

			// this also applies for the simple predicates of the form "ident op const" or "ident op const-array"
			pexprBitmapLocal = PexprBitmapSelectBestIndex(
				pmp, pmda, pexprPred, ptabdesc, pmdrel, pdrgpcrOutput,
				pcrsOuterRefs, &pexprRecheckLocal, &pexprResidualLocal,
				isAPartialPredicateOrArrayCmp  // for partial preds or array comps
				// we want to consider btree/hash indexes
			);

			// since we did not break the conjunct tree, the index path found may cover a part of the
			// predicate only, hence we perform the index lookup again for the residual.

			// example, with schema as t(a, b, c, f, e), indexes as i1(a, b), i2(a) and i3(e, f)
			// for predicate: (a = 40) AND (b = 3) AND (e = 11) AND (c = 34)
			// first lookup will produce index path (i1) with residual as: (e = 11) AND (c = 34)
			// retrying with this residual will produce second index path (i3) with final residual: (c = 34).
			// the two index paths will be joined at the end.
			retryIndexLookupWithResidual = true;
		}
		else
		{
			// break the predicate down and look for index paths on individual children
			pexprBitmapLocal = PexprBitmapLookupWithPredicateBreakDown(
				pmp, pmda, pexprOriginalPred, pexprPred, ptabdesc, pmdrel,
				pdrgpcrOutput, pcrsOuterRefs, pcrsReqd, &pexprRecheckLocal,
				&pexprResidualLocal);
			// if no index path was constructed for this predicate, return it as residual.
			// Example, with schema: t(a, b, c, d), index i1(a,b)
			// and predicate: (a = 3) OR (c = 4 AND d =5)
			// no index path will be found for (c = 4 AND d =5), in which case the entire
			// disjunct will become a residual.
			if (nullptr == pexprBitmapLocal)
			{
				pexprPred->AddRef();
				pexprResidualLocal = pexprPred;
			}
		}

		CRefCount::SafeRelease(pexprPred);

		if (nullptr != pexprBitmapLocal)
		{
			GPOS_ASSERT(nullptr != pexprRecheckLocal);

			pdrgpexprRecheckTemp->Append(pexprRecheckLocal);
			pdrgpexprBitmapTemp->Append(pexprBitmapLocal);
		}

		if (nullptr != pexprBitmapLocal && retryIndexLookupWithResidual)
		{
			// if an index path was found, then perform the lookup again for the residual
			pexprPred = pexprResidualLocal;
			continue;
		}

		// terminate the lookup if no index path was found or
		// if the retry was not required
		pexprPred = nullptr;
	}

	// for simple conjuncts, there may be multiple index paths generated by CreateBitmapIndexProbesWithOrWithoutPredBreakdown()
	// due to the retry, in that case we combine them with BitmapAnd expression.
	// for example, if you have index foo_a and foo_b with a predicate (a = 1 and b = 2 and c = 3)
	// pdrgpexprBitmapTemp will return [foo_a, foo_b]
	// pdrgpexprRecheckTemp will give recheck conditions [a = 1, b = 2]
	// these can then be ANDed together below before being appended as the final bitmap and recheck conditions
	// needed for the index probe.
	const ULONG ulBitmapExpr = pdrgpexprBitmapTemp->Size();
	if (0 < ulBitmapExpr)
	{
		JoinBitmapIndexProbes(pmp, pdrgpexprBitmapTemp, pdrgpexprRecheckTemp,
							  true /*fConjunction*/, pexprBitmapResult,
							  pexprRecheckResult);
	}

	if (nullptr != pexprResidualLocal)
	{
		pdrgpexprResidualResult->Append(pexprResidualLocal);
		// Note that since we dont have an fConjunction parameter,
		// adding the residuals to the list might be incorrect, if we are
		// inside an OR predicate. We do this anyway and rely on logic in
		// ComputeBitmapTableScanResidualPredicate(), called from
		// PexprScalarBitmapBoolOp() to replace these jumbled predicates
		// with the entire original predicate as a residual predicate.
	}

	// cleanup
	CRefCount::SafeRelease(pdrgpexprBitmapTemp);
	CRefCount::SafeRelease(pdrgpexprRecheckTemp);
}

// combine the individual bitmap access paths to form a bitmap bool op expression
void
CXformUtils::JoinBitmapIndexProbes(CMemoryPool *pmp,
								   CExpressionArray *pdrgpexprBitmap,
								   CExpressionArray *pdrgpexprRecheck,
								   BOOL fConjunction,
								   CExpression **ppexprBitmap,
								   CExpression **ppexprRecheck)
{
	const ULONG ulBitmapExpr = pdrgpexprBitmap->Size();
	CExpression *pexprBitmapBoolOp = (*pdrgpexprBitmap)[0];
	pexprBitmapBoolOp->AddRef();
	IMDId *pmdidBitmap =
		CScalar::PopConvert(pexprBitmapBoolOp->Pop())->MdidType();

	for (ULONG ul = 1; ul < ulBitmapExpr; ul++)
	{
		CExpression *pexprBitmap = (*pdrgpexprBitmap)[ul];
		pexprBitmap->AddRef();
		pmdidBitmap->AddRef();

		pexprBitmapBoolOp = PexprBitmapBoolOp(
			pmp, pmdidBitmap, pexprBitmapBoolOp, pexprBitmap, fConjunction);
	}

	GPOS_ASSERT(nullptr != pexprBitmapBoolOp && 0 < pdrgpexprRecheck->Size());
	(*ppexprBitmap) = pexprBitmapBoolOp;

	pdrgpexprRecheck->AddRef();
	CExpression *pexprRecheckNew =
		CPredicateUtils::PexprConjDisj(pmp, pdrgpexprRecheck, fConjunction);
	(*ppexprRecheck) = pexprRecheckNew;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FHasAmbiguousType
//
//	@doc:
//		Check if expression has a scalar node with ambiguous return type
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FHasAmbiguousType(CExpression *pexpr, CMDAccessor *md_accessor)
{
	GPOS_CHECK_STACK_SIZE;
	GPOS_ASSERT(nullptr != pexpr);
	GPOS_ASSERT(nullptr != md_accessor);

	BOOL fAmbiguous = false;
	if (pexpr->Pop()->FScalar())
	{
		CScalar *popScalar = CScalar::PopConvert(pexpr->Pop());
		switch (popScalar->Eopid())
		{
			case COperator::EopScalarAggFunc:
				fAmbiguous = CScalarAggFunc::PopConvert(popScalar)
								 ->FHasAmbiguousReturnType();
				break;

			case COperator::EopScalarProjectList:
			case COperator::EopScalarProjectElement:
			case COperator::EopScalarSwitchCase:
			case COperator::EopScalarArrayRefIndexList:
				break;	// these operators do not have valid return type

			default:
				IMDId *mdid = popScalar->MdidType();
				if (nullptr != mdid)
				{
					// check MD type of scalar node
					fAmbiguous = md_accessor->RetrieveType(mdid)->IsAmbiguous();
				}
		}
	}

	if (!fAmbiguous)
	{
		// recursively process children
		const ULONG arity = pexpr->Arity();
		for (ULONG ul = 0; !fAmbiguous && ul < arity; ul++)
		{
			CExpression *pexprChild = (*pexpr)[ul];
			fAmbiguous = FHasAmbiguousType(pexprChild, md_accessor);
		}
	}

	return fAmbiguous;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprSelect2BitmapBoolOp
//
//	@doc:
//		Transform a Select into a Bitmap(Dynamic)TableGet over BitmapBoolOp if
//		bitmap indexes exist
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprSelect2BitmapBoolOp(CMemoryPool *mp, CExpression *pexpr)
{
	GPOS_ASSERT(nullptr != pexpr);

	// extract components
	CExpression *pexprRelational = (*pexpr)[0];
	CExpression *pexprScalar = (*pexpr)[1];
	CLogical *popGet = CLogical::PopConvert(pexprRelational->Pop());

	CTableDescriptorHashSet *ptabdescset =
		pexprRelational->DeriveTableDescriptor();
	GPOS_ASSERT(1 == ptabdescset->Size());
	CTableDescriptor *ptabdesc = ptabdescset->First();

	GPOS_ASSERT(nullptr != ptabdesc);
	const ULONG ulIndices = ptabdesc->IndexCount();
	if (0 == ulIndices)
	{
		return nullptr;
	}

	// derive the scalar and relational properties to build set of required columns
	CColRefSet *pcrsOutput = pexpr->DeriveOutputColumns();
	CColRefSet *pcrsScalarExpr = pexprScalar->DeriveUsedColumns();

	CColRefSet *pcrsReqd = GPOS_NEW(mp) CColRefSet(mp);
	pcrsReqd->Include(pcrsOutput);
	pcrsReqd->Include(pcrsScalarExpr);

	CExpression *pexprResult = PexprBitmapTableGet(
		mp, popGet, pexpr->Pop()->UlOpId(), ptabdesc, pexprScalar,
		nullptr,  // outer_refs
		pcrsReqd);
	pcrsReqd->Release();

	return pexprResult;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprBitmapTableGet
//
//	@doc:
//		Transform a Select into a Bitmap(Dynamic)TableGet over BitmapBoolOp if
//		bitmap indexes exist
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprBitmapTableGet(CMemoryPool *mp, CLogical *popGet,
								 ULONG ulOriginOpId, CTableDescriptor *ptabdesc,
								 CExpression *pexprScalar,
								 CColRefSet *outer_refs, CColRefSet *pcrsReqd)
{
	GPOS_ASSERT(COperator::EopLogicalGet == popGet->Eopid() ||
				COperator::EopLogicalDynamicGet == popGet->Eopid());

	BOOL fDynamicGet = (COperator::EopLogicalDynamicGet == popGet->Eopid());

	BOOL fConjunction = CPredicateUtils::FAnd(pexprScalar);

	CExpressionArray *pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
	pexprScalar->AddRef();
	pdrgpexpr->Append(pexprScalar);


	GPOS_ASSERT(0 < pdrgpexpr->Size());

	// find the indexes whose included columns meet the required columns
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	const IMDRelation *pmdrel = md_accessor->RetrieveRel(ptabdesc->MDId());

	GPOS_ASSERT(0 < pdrgpexpr->Size());

	CColRefArray *pdrgpcrOutput = CLogical::PdrgpcrOutputFromLogicalGet(popGet);
	GPOS_ASSERT(nullptr != pdrgpcrOutput);

	CExpression *pexprRecheck = nullptr;
	CExpression *pexprResidual = nullptr;
	CExpression *pexprBitmap = PexprScalarBitmapBoolOp(
		mp, md_accessor, pexprScalar, pdrgpexpr, ptabdesc, pmdrel,
		pdrgpcrOutput, outer_refs, pcrsReqd, fConjunction, &pexprRecheck,
		&pexprResidual, false /*isAPartialPredicate*/
	);
	CExpression *pexprResult = nullptr;

	if (nullptr != pexprBitmap)
	{
		GPOS_ASSERT(nullptr != pexprRecheck);
		ptabdesc->AddRef();
		pdrgpcrOutput->AddRef();

		CName *pname = GPOS_NEW(mp)
			CName(mp, CName(CLogical::NameFromLogicalGet(popGet).Pstr()));

		// create a bitmap table scan on top
		CLogical *popBitmapTableGet = nullptr;

		if (fDynamicGet)
		{
			CLogicalDynamicGet *popDynamicGet =
				CLogicalDynamicGet::PopConvert(popGet);
			popDynamicGet->PdrgpdrgpcrPart()->AddRef();
			popDynamicGet->GetPartitionMdids()->AddRef();
			popBitmapTableGet = GPOS_NEW(mp) CLogicalDynamicBitmapTableGet(
				mp, ptabdesc, ulOriginOpId, pname, popDynamicGet->ScanId(),
				pdrgpcrOutput, popDynamicGet->PdrgpdrgpcrPart(),
				popDynamicGet->GetPartitionMdids());
		}
		else
		{
			popBitmapTableGet = GPOS_NEW(mp) CLogicalBitmapTableGet(
				mp, ptabdesc, ulOriginOpId, pname, pdrgpcrOutput);
		}
		pexprResult = GPOS_NEW(mp)
			CExpression(mp, popBitmapTableGet, pexprRecheck, pexprBitmap);

		if (nullptr != pexprResidual)
		{
			// add a selection on top with the residual condition
			pexprResult =
				GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CLogicalSelect(mp),
										 pexprResult, pexprResidual);
		}
	}

	// cleanup
	pdrgpexpr->Release();

	return pexprResult;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprRemapColumns
//
//	@doc:
//		Remap the expression from the old columns to the new ones
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprRemapColumns(CMemoryPool *mp, CExpression *pexpr,
							   CColRefArray *pdrgpcrA,
							   CColRefArray *pdrgpcrRemappedA,
							   CColRefArray *pdrgpcrB,
							   CColRefArray *pdrgpcrRemappedB)
{
	UlongToColRefMap *colref_mapping =
		CUtils::PhmulcrMapping(mp, pdrgpcrA, pdrgpcrRemappedA);
	GPOS_ASSERT_IMP(nullptr == pdrgpcrB, nullptr == pdrgpcrRemappedB);
	if (nullptr != pdrgpcrB)
	{
		CUtils::AddColumnMapping(mp, colref_mapping, pdrgpcrB,
								 pdrgpcrRemappedB);
	}
	CExpression *pexprRemapped = pexpr->PexprCopyWithRemappedColumns(
		mp, colref_mapping, true /*must_exist*/);
	colref_mapping->Release();

	return pexprRemapped;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::FJoinPredOnSingleChild
//
//	@doc:
//		Check if expression handle is attached to a Join expression
//		with a join predicate that uses columns from a single child
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FJoinPredOnSingleChild(CMemoryPool *mp, CExpressionHandle &exprhdl)
{
	GPOS_ASSERT(CUtils::FLogicalJoin(exprhdl.Pop()));

	const ULONG arity = exprhdl.Arity();
	if (0 == exprhdl.DeriveUsedColumns(arity - 1)->Size())
	{
		// no columns are used in join predicate
		return false;
	}

	// construct array of children output columns
	CColRefSetArray *pdrgpcrs = GPOS_NEW(mp) CColRefSetArray(mp);
	for (ULONG ul = 0; ul < arity - 1; ul++)
	{
		CColRefSet *pcrsOutput = exprhdl.DeriveOutputColumns(ul);
		pcrsOutput->AddRef();
		pdrgpcrs->Append(pcrsOutput);
	}

	CExpressionArray *pdrgpexprPreds = CPredicateUtils::PdrgpexprConjuncts(
		mp, exprhdl.PexprScalarExactChild(arity - 1,
										  true /*error_on_null_return*/));
	const ULONG ulPreds = pdrgpexprPreds->Size();
	BOOL fPredUsesSingleChild = false;
	for (ULONG ulPred = 0; !fPredUsesSingleChild && ulPred < ulPreds; ulPred++)
	{
		CExpression *pexpr = (*pdrgpexprPreds)[ulPred];
		CColRefSet *pcrsUsed = pexpr->DeriveUsedColumns();
		for (ULONG ulChild = 0; !fPredUsesSingleChild && ulChild < arity - 1;
			 ulChild++)
		{
			fPredUsesSingleChild = (*pdrgpcrs)[ulChild]->ContainsAll(pcrsUsed);
		}
	}
	pdrgpexprPreds->Release();
	pdrgpcrs->Release();

	return fPredUsesSingleChild;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprCTEConsumer
//
//	@doc:
//		Create a new CTE consumer for the given CTE id
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprCTEConsumer(CMemoryPool *mp, ULONG ulCTEId,
							  CColRefArray *colref_array)
{
	CExpression *pexpr;
	CLogicalCTEConsumer *popConsumer =
		GPOS_NEW(mp) CLogicalCTEConsumer(mp, ulCTEId, colref_array);

	pexpr = GPOS_NEW(mp) CExpression(mp, popConsumer);
	COptCtxt::PoctxtFromTLS()->Pcteinfo()->IncrementConsumers(ulCTEId);
	COptCtxt::PoctxtFromTLS()->Pcteinfo()->AddCTEConsumer(pexpr);
	return pexpr;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PdrgpcrSubsequence
//
//	@doc:
//		Returns a new array containing the columns from the given column array 'colref_array'
//		at the positions indicated by the given ULONG array 'pdrgpulIndexesOfRefs'
//
//---------------------------------------------------------------------------
CColRefArray *
CXformUtils::PdrgpcrReorderedSubsequence(CMemoryPool *mp,
										 CColRefArray *colref_array,
										 ULongPtrArray *pdrgpulIndexesOfRefs)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(nullptr != pdrgpulIndexesOfRefs);

	const ULONG length = pdrgpulIndexesOfRefs->Size();
	GPOS_ASSERT(length <= colref_array->Size());

	CColRefArray *pdrgpcrNewSubsequence = GPOS_NEW(mp) CColRefArray(mp);
	for (ULONG ul = 0; ul < length; ul++)
	{
		ULONG ulPos = *(*pdrgpulIndexesOfRefs)[ul];
		GPOS_ASSERT(ulPos < colref_array->Size());
		pdrgpcrNewSubsequence->Append((*colref_array)[ulPos]);
	}

	return pdrgpcrNewSubsequence;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprWinFuncAgg2ScalarAgg
//
//	@doc:
//		Converts an Agg window function into regular Agg
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprWinFuncAgg2ScalarAgg(CMemoryPool *mp,
									   CExpression *pexprWinFunc)
{
	GPOS_ASSERT(nullptr != pexprWinFunc);
	GPOS_ASSERT(COperator::EopScalarWindowFunc == pexprWinFunc->Pop()->Eopid());

	CExpressionArray *pdrgpexprFullWinFuncArgs =
		GPOS_NEW(mp) CExpressionArray(mp);

	CExpressionArray *pdrgpexprWinFuncArgs = GPOS_NEW(mp) CExpressionArray(mp);
	const ULONG ulArgs = pexprWinFunc->Arity();
	for (ULONG ul = 0; ul < ulArgs; ul++)
	{
		CExpression *pexprArg = (*pexprWinFunc)[ul];
		pexprArg->AddRef();
		pdrgpexprWinFuncArgs->Append(pexprArg);
	}

	pdrgpexprFullWinFuncArgs->Append(GPOS_NEW(mp) CExpression(
		mp, GPOS_NEW(mp) CScalarValuesList(mp), pdrgpexprWinFuncArgs));

	pdrgpexprFullWinFuncArgs->Append(
		GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CScalarValuesList(mp),
								 GPOS_NEW(mp) CExpressionArray(mp)));
	pdrgpexprFullWinFuncArgs->Append(
		GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CScalarValuesList(mp),
								 GPOS_NEW(mp) CExpressionArray(mp)));
	pdrgpexprFullWinFuncArgs->Append(
		GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CScalarValuesList(mp),
								 GPOS_NEW(mp) CExpressionArray(mp)));

	CScalarWindowFunc *popScWinFunc =
		CScalarWindowFunc::PopConvert(pexprWinFunc->Pop());
	IMDId *mdid_func = popScWinFunc->FuncMdId();

	mdid_func->AddRef();
	return GPOS_NEW(mp) CExpression(
		mp,
		CUtils::PopAggFunc(
			mp, mdid_func,
			GPOS_NEW(mp)
				CWStringConst(mp, popScWinFunc->PstrFunc()->GetBuffer()),
			popScWinFunc->IsDistinct(), EaggfuncstageGlobal,
			false,	  // fSplit
			nullptr,  // pmdidResolvedReturnType
			EaggfunckindNormal, GPOS_NEW(mp) ULongPtrArray(mp), false, popScWinFunc->IsStarArg()),
		pdrgpexprFullWinFuncArgs);
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::MapPrjElemsWithDistinctAggs
//
//	@doc:
//		Given a project list, create a map whose key is the argument of
//		distinct Agg, and value is the set of project elements that define
//		distinct Aggs on that argument,
//		non-distinct Aggs are grouped together in one set with key 'True',
//		for example,
//
//		Input: (x : count(distinct a),
//				y : sum(distinct a),
//				z : avg(distinct b),
//				w : max(c))
//
//		Output: (a --> {x,y},
//				 b --> {z},
//				 true --> {w})
//
//---------------------------------------------------------------------------
void
CXformUtils::MapPrjElemsWithDistinctAggs(
	CMemoryPool *mp, CExpression *pexprPrjList,
	ExprToExprArrayMap **pphmexprdrgpexpr,	// output: created map
	ULONG *pulDifferentDQAs	 // output: number of DQAs with different arguments
)
{
	GPOS_ASSERT(nullptr != pexprPrjList);
	GPOS_ASSERT(nullptr != pphmexprdrgpexpr);
	GPOS_ASSERT(nullptr != pulDifferentDQAs);

	ExprToExprArrayMap *phmexprdrgpexpr = GPOS_NEW(mp) ExprToExprArrayMap(mp);
	ULONG ulDifferentDQAs = 0;
	CExpression *pexprTrue = CUtils::PexprScalarConstBool(mp, true /*value*/);
	const ULONG arity = pexprPrjList->Arity();
	for (ULONG ul = 0; ul < arity; ul++)
	{
		CExpression *pexprPrjEl = (*pexprPrjList)[ul];
		CExpression *pexprChild = (*pexprPrjEl)[0];
		COperator *popChild = pexprChild->Pop();
		COperator::EOperatorId eopidChild = popChild->Eopid();
		if (COperator::EopScalarAggFunc != eopidChild &&
			COperator::EopScalarWindowFunc != eopidChild)
		{
			continue;
		}

		BOOL is_distinct = false;
		if (COperator::EopScalarAggFunc == eopidChild)
		{
			is_distinct = CScalarAggFunc::PopConvert(popChild)->IsDistinct();
		}
		else
		{
			is_distinct = CScalarWindowFunc::PopConvert(popChild)->IsDistinct();
		}

		CExpression *pexprKey = nullptr;
		if (is_distinct && 1 == pexprChild->Arity())
		{
			// use first argument of Distinct Agg as key
			pexprKey = (*pexprChild)[0];
		}
		else if (is_distinct && COperator::EopScalarAggFunc == eopidChild)
		{
			// use first argument of AggFunc args list as key
			pexprKey = (*(pexprChild->PdrgPexpr()))[EaggfuncIndexArgs];
		}
		else
		{
			// use constant True as key
			pexprKey = pexprTrue;
		}

		CExpressionArray *pdrgpexpr = phmexprdrgpexpr->Find(pexprKey);
		BOOL fExists = (nullptr != pdrgpexpr);
		if (!fExists)
		{
			// first occurrence, create a new expression array
			pdrgpexpr = GPOS_NEW(mp) CExpressionArray(mp);
		}
		pexprPrjEl->AddRef();
		pdrgpexpr->Append(pexprPrjEl);

		if (!fExists)
		{
			pexprKey->AddRef();
#ifdef GPOS_DEBUG
			BOOL fSuccess =
#endif	// GPOS_DEBUG
				phmexprdrgpexpr->Insert(pexprKey, pdrgpexpr);
			GPOS_ASSERT(fSuccess);

			if (pexprKey != pexprTrue)
			{
				// first occurrence of a new DQA, increment counter
				ulDifferentDQAs++;
			}
		}
	}

	pexprTrue->Release();

	*pphmexprdrgpexpr = phmexprdrgpexpr;
	*pulDifferentDQAs = ulDifferentDQAs;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::ICmpPrjElemsArr
//
//	@doc:
//		Comparator used in sorting arrays of project elements
//		based on the column id of the first entry
//
//---------------------------------------------------------------------------
INT
CXformUtils::ICmpPrjElemsArr(const void *pvFst, const void *pvSnd)
{
	GPOS_ASSERT(nullptr != pvFst);
	GPOS_ASSERT(nullptr != pvSnd);

	const CExpressionArray *pdrgpexprFst = *(const CExpressionArray **) (pvFst);
	const CExpressionArray *pdrgpexprSnd = *(const CExpressionArray **) (pvSnd);

	CExpression *pexprPrjElemFst = (*pdrgpexprFst)[0];
	CExpression *pexprPrjElemSnd = (*pdrgpexprSnd)[0];
	ULONG ulIdFst =
		CScalarProjectElement::PopConvert(pexprPrjElemFst->Pop())->Pcr()->Id();
	ULONG ulIdSnd =
		CScalarProjectElement::PopConvert(pexprPrjElemSnd->Pop())->Pcr()->Id();

	if (ulIdFst < ulIdSnd)
	{
		return -1;
	}

	if (ulIdFst > ulIdSnd)
	{
		return 1;
	}

	return 0;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PdrgpdrgpexprSortedPrjElemsArray
//
//	@doc:
//		Iterate over given hash map and return array of arrays of project
//		elements sorted by the column id of the first entries
//
//---------------------------------------------------------------------------
CExpressionArrays *
CXformUtils::PdrgpdrgpexprSortedPrjElemsArray(
	CMemoryPool *mp, ExprToExprArrayMap *phmexprdrgpexpr)
{
	GPOS_ASSERT(nullptr != phmexprdrgpexpr);

	CExpressionArrays *pdrgpdrgpexprPrjElems =
		GPOS_NEW(mp) CExpressionArrays(mp);
	ExprToExprArrayMapIter hmexprdrgpexpriter(phmexprdrgpexpr);
	while (hmexprdrgpexpriter.Advance())
	{
		CExpressionArray *pdrgpexprPrjElems =
			const_cast<CExpressionArray *>(hmexprdrgpexpriter.Value());
		pdrgpexprPrjElems->AddRef();
		pdrgpdrgpexprPrjElems->Append(pdrgpexprPrjElems);
	}
	pdrgpdrgpexprPrjElems->Sort(ICmpPrjElemsArr);

	return pdrgpdrgpexprPrjElems;
}


//---------------------------------------------------------------------------
//	@function:
//		CXformUtils::PexprGbAggOnCTEConsumer2Join
//
//	@doc:
//		Convert GbAgg with distinct aggregates to a join expression
//
//		each leaf node of the resulting join expression is a GbAgg on a single
//		distinct aggs, we also create a GbAgg leaf for all remaining (non-distinct)
//		aggregates, for example
//
//		Input:
//			GbAgg_(count(distinct a), max(distinct a), sum(distinct b), avg(a))
//				+---CTEConsumer
//
//		Output:
//			InnerJoin
//				|--InnerJoin
//				|		|--GbAgg_(count(distinct a), max(distinct a))
//				|		|		+---CTEConsumer
//				|		+--GbAgg_(sum(distinct b))
//				|				+---CTEConsumer
//				+--GbAgg_(avg(a))
//						+---CTEConsumer
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::PexprGbAggOnCTEConsumer2Join(CMemoryPool *mp,
										  CExpression *pexprGbAgg)
{
	GPOS_ASSERT(nullptr != pexprGbAgg);
	GPOS_ASSERT(COperator::EopLogicalGbAgg == pexprGbAgg->Pop()->Eopid());

	CLogicalGbAgg *popGbAgg = CLogicalGbAgg::PopConvert(pexprGbAgg->Pop());
	CColRefArray *pdrgpcrGrpCols = popGbAgg->Pdrgpcr();

	GPOS_ASSERT(popGbAgg->FGlobal());

	if (COperator::EopLogicalCTEConsumer != (*pexprGbAgg)[0]->Pop()->Eopid())
	{
		// child of GbAgg must be a CTE consumer

		return nullptr;
	}

	CExpression *pexprPrjList = (*pexprGbAgg)[1];
	ULONG ulDistinctAggs = pexprPrjList->DeriveTotalDistinctAggs();

	if (1 == ulDistinctAggs)
	{
		// if only one distinct agg is used, return input expression
		pexprGbAgg->AddRef();
		return pexprGbAgg;
	}

	ExprToExprArrayMap *phmexprdrgpexpr = nullptr;
	ULONG ulDifferentDQAs = 0;
	MapPrjElemsWithDistinctAggs(mp, pexprPrjList, &phmexprdrgpexpr,
								&ulDifferentDQAs);
	if (1 == phmexprdrgpexpr->Size())
	{
		// if all distinct aggs use the same argument, return input expression
		phmexprdrgpexpr->Release();

		pexprGbAgg->AddRef();
		return pexprGbAgg;
	}

	CExpression *pexprCTEConsumer = (*pexprGbAgg)[0];
	CLogicalCTEConsumer *popConsumer =
		CLogicalCTEConsumer::PopConvert(pexprCTEConsumer->Pop());
	const ULONG ulCTEId = popConsumer->UlCTEId();
	CColRefArray *pdrgpcrConsumerOutput = popConsumer->Pdrgpcr();
	CCTEInfo *pcteinfo = COptCtxt::PoctxtFromTLS()->Pcteinfo();

	CExpression *pexprLastGbAgg = nullptr;
	CColRefArray *pdrgpcrLastGrpCols = nullptr;
	CExpression *pexprJoin = nullptr;
	CExpression *pexprTrue = CUtils::PexprScalarConstBool(mp, true /*value*/);

	// iterate over map to extract sorted array of array of project elements,
	// we need to sort arrays here since hash map iteration is non-deterministic,
	// which may create non-deterministic ordering of join children leading to
	// changing the plan of the same query when run multiple times
	CExpressionArrays *pdrgpdrgpexprPrjElems =
		PdrgpdrgpexprSortedPrjElemsArray(mp, phmexprdrgpexpr);

	// counter of consumers
	ULONG ulConsumers = 0;

	const ULONG size = pdrgpdrgpexprPrjElems->Size();
	for (ULONG ulPrjElemsArr = 0; ulPrjElemsArr < size; ulPrjElemsArr++)
	{
		CExpressionArray *pdrgpexprPrjElems =
			(*pdrgpdrgpexprPrjElems)[ulPrjElemsArr];

		CExpression *pexprNewGbAgg = nullptr;
		if (0 == ulConsumers)
		{
			// reuse input consumer
			pdrgpcrGrpCols->AddRef();
			pexprCTEConsumer->AddRef();
			pdrgpexprPrjElems->AddRef();
			pexprNewGbAgg = GPOS_NEW(mp) CExpression(
				mp,
				GPOS_NEW(mp) CLogicalGbAgg(mp, pdrgpcrGrpCols,
										   COperator::EgbaggtypeGlobal),
				pexprCTEConsumer,
				GPOS_NEW(mp)
					CExpression(mp, GPOS_NEW(mp) CScalarProjectList(mp),
								pdrgpexprPrjElems));
		}
		else
		{
			// create a new consumer
			CColRefArray *pdrgpcrNewConsumerOutput =
				CUtils::PdrgpcrCopy(mp, pdrgpcrConsumerOutput);
			CExpression *pexprNewConsumer = GPOS_NEW(mp)
				CExpression(mp, GPOS_NEW(mp) CLogicalCTEConsumer(
									mp, ulCTEId, pdrgpcrNewConsumerOutput));
			pcteinfo->IncrementConsumers(ulCTEId);
			pcteinfo->AddCTEConsumer(pexprNewConsumer);

			// fix Aggs arguments to use new consumer output column
			UlongToColRefMap *colref_mapping = CUtils::PhmulcrMapping(
				mp, pdrgpcrConsumerOutput, pdrgpcrNewConsumerOutput);
			CExpressionArray *pdrgpexprNewPrjElems =
				GPOS_NEW(mp) CExpressionArray(mp);
			const ULONG ulPrjElems = pdrgpexprPrjElems->Size();
			for (ULONG ul = 0; ul < ulPrjElems; ul++)
			{
				CExpression *pexprPrjEl = (*pdrgpexprPrjElems)[ul];

				// to match requested columns upstream, we have to re-use the same computed
				// columns that define the aggregates, we avoid creating new columns during
				// expression copy by passing must_exist as false
				CExpression *pexprNewPrjEl =
					pexprPrjEl->PexprCopyWithRemappedColumns(
						mp, colref_mapping, false /*must_exist*/);
				pdrgpexprNewPrjElems->Append(pexprNewPrjEl);
			}

			// re-map grouping columns
			CColRefArray *pdrgpcrNewGrpCols = CUtils::PdrgpcrRemap(
				mp, pdrgpcrGrpCols, colref_mapping, true /*must_exist*/);

			// create new GbAgg expression
			pexprNewGbAgg = GPOS_NEW(mp) CExpression(
				mp,
				GPOS_NEW(mp) CLogicalGbAgg(mp, pdrgpcrNewGrpCols,
										   COperator::EgbaggtypeGlobal),
				pexprNewConsumer,
				GPOS_NEW(mp)
					CExpression(mp, GPOS_NEW(mp) CScalarProjectList(mp),
								pdrgpexprNewPrjElems));

			colref_mapping->Release();
		}

		ulConsumers++;

		CColRefArray *pdrgpcrNewGrpCols =
			CLogicalGbAgg::PopConvert(pexprNewGbAgg->Pop())->Pdrgpcr();
		if (nullptr != pexprLastGbAgg)
		{
			CExpression *pexprJoinCondition = nullptr;
			if (0 == pdrgpcrLastGrpCols->Size())
			{
				GPOS_ASSERT(0 == pdrgpcrNewGrpCols->Size());

				pexprTrue->AddRef();
				pexprJoinCondition = pexprTrue;
			}
			else
			{
				GPOS_ASSERT(pdrgpcrLastGrpCols->Size() ==
							pdrgpcrNewGrpCols->Size());

				pexprJoinCondition = CPredicateUtils::PexprINDFConjunction(
					mp, pdrgpcrLastGrpCols, pdrgpcrNewGrpCols);
			}

			if (nullptr == pexprJoin)
			{
				// create first join
				pexprJoin = CUtils::PexprLogicalJoin<CLogicalInnerJoin>(
					mp, pexprLastGbAgg, pexprNewGbAgg, pexprJoinCondition);
			}
			else
			{
				// cascade joins
				pexprJoin = CUtils::PexprLogicalJoin<CLogicalInnerJoin>(
					mp, pexprJoin, pexprNewGbAgg, pexprJoinCondition);
			}
		}

		pexprLastGbAgg = pexprNewGbAgg;
		pdrgpcrLastGrpCols = pdrgpcrNewGrpCols;
	}

	pdrgpdrgpexprPrjElems->Release();
	phmexprdrgpexpr->Release();
	pexprTrue->Release();

	return pexprJoin;
}

//---------------------------------------------------------------------------
// CXformUtils::AddALinearStackOfUnaryExpressions
//
// Given two CExpressions, a "lower part" and a "stack", consisting of
// zero or more CExpressions with a single logical child and optional scalar
// children, make a copy of the stack, with the lower (excluded) end of the
// stack ("exclusiveBottomOfStack") replaced by "lowerPartOfExpr".
//
// Input:
//
//      lowerPartOfExpr             topOfStack
//          / | | \                     |
//    (optional children)              ...
//                                      |
//                               lastIncludedNode
//                                      |
//                             exclusiveBottomOfStack
//
// Result:
//
//                topOfStack
//                    |
//                   ...
//                    |
//             lastIncludedNode
//                    |
//              lowerPartOfExpr
//                  / | | \ .
//            (optional children)
//
//---------------------------------------------------------------------------
CExpression *
CXformUtils::AddALinearStackOfUnaryExpressions(
	CMemoryPool *mp, CExpression *lowerPartOfExpr, CExpression *topOfStack,
	CExpression *exclusiveBottomOfStack)
{
	if (nullptr == topOfStack || topOfStack == exclusiveBottomOfStack)
	{
		// nothing to add on top of lowerPartOfExpr
		return lowerPartOfExpr;
	}

	ULONG arity = topOfStack->Arity();

	// a stack must consist of logical nodes
	GPOS_ASSERT(topOfStack->Pop()->FLogical());
	GPOS_CHECK_STACK_SIZE;

	// Recursively process the node just below topOfStack first, to build the new stack bottom-up.
	// Note that if the stack ends here, the recursive call will return lowerPartOfExpr.
	CExpression *processedRestOfStack = AddALinearStackOfUnaryExpressions(
		mp, lowerPartOfExpr, (*topOfStack)[0], exclusiveBottomOfStack);

	// now add a copy of node topOfStack
	CExpressionArray *childrenArray = GPOS_NEW(mp) CExpressionArray(mp);
	COperator *pop = topOfStack->Pop();

	// the first, logical child becomes the copied rest of the stack
	childrenArray->Append(processedRestOfStack);

	// then copy the remaining (scalar) children, unmodified
	for (ULONG ul = 1; ul < arity; ul++)
	{
		CExpression *scalarChild = (*topOfStack)[ul];

		GPOS_ASSERT(scalarChild->Pop()->FScalar());
		scalarChild->AddRef();
		childrenArray->Append(scalarChild);
	}

	pop->AddRef();

	return GPOS_NEW(mp) CExpression(mp, pop, childrenArray);
}

//---------------------------------------------------------------------------
// CXformUtils::FCoverIndex
//
// Given an index descriptor, a table descriptor, and an output columnn array
// return whether the index covers the output column array.
//
//---------------------------------------------------------------------------
BOOL
CXformUtils::FCoverIndex(CMemoryPool *mp, CIndexDescriptor *pindexdesc,
						 CTableDescriptor *ptabdesc,
						 CColRefArray *pdrgpcrOutput)
{
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();
	const IMDRelation *pmdrel = md_accessor->RetrieveRel(ptabdesc->MDId());
	const IMDIndex *pmdindex = md_accessor->RetrieveIndex(pindexdesc->MDId());

	GPOS_ASSERT(nullptr != pdrgpcrOutput);
	pdrgpcrOutput->AddRef();

	CColRefSet *returnable_cols = CXformUtils::PcrsIndexReturnableColumns(
		mp, pdrgpcrOutput, pmdindex, pmdrel);
	CColRefSet *output_cols = GPOS_NEW(mp) CColRefSet(mp);

	// An index only scan is allowed iff each used output column reference also
	// exists as a column in the index.
	for (ULONG i = 0; i < pdrgpcrOutput->Size(); i++)
	{
		CColRef *col = (*pdrgpcrOutput)[i];

		// In most cases we want to treat system columns unconditionally as
		// used. This is because certain transforms like those for DML or
		// CXformPushGbBelowJoin use unique keys in the derived properties,
		// even if they are not referenced in the query. Those keys are system
		// columns gp_segment_id and ctid. We also treat distribution columns
		// as used, since they appear in the CDistributionSpecHashed of
		// physical properties and therefore might be used in the plan.
		//
		// NB: Because 'pexpr' is not a scalar expression, we cannot derive
		// scalar properties (e.g. PcrsUsed/DeriveUsedColumns). So instead, we
		// check the used columns via GetUsage. DeriveOutputColumns could also
		// work, but would need a flag to include system/distribution columns.
		if (col->GetUsage(true /*check_system_cols*/,
						  true /*check_distribution_col*/) == CColRef::EUsed)
		{
			output_cols->Include(col);
		}
	}

	if (!returnable_cols->ContainsAll(output_cols))
	{
		returnable_cols->Release();
		output_cols->Release();
		pdrgpcrOutput->Release();
		return false;
	}
	returnable_cols->Release();
	output_cols->Release();

	return true;
}

//---------------------------------------------------------------------------
// CXformUtils::ComputeOrderSpecForIndexKey
//
// Determine the OrderSpec for an index key based on the direction of
// the index scan and the position of the index key. This function
// creates a new OrderSpec object and computes its value if a nullptr is
// passed; otherwise, it appends to the existing OrderSpec object.
//---------------------------------------------------------------------------
void
CXformUtils::ComputeOrderSpecForIndexKey(CMemoryPool *mp,
										 COrderSpec **order_spec,
										 const IMDIndex *pmdindex,
										 EIndexScanDirection scan_direction,
										 const CColRef *colref,
										 ULONG key_position)
{
	// Create a new OrderSpec object if a nullptr is passed
	if (*order_spec == nullptr)
	{
		(*order_spec) = GPOS_NEW(mp) COrderSpec(mp);
	}
	IMDId *mdid = nullptr;
	COrderSpec::ENullTreatment ent = COrderSpec::EntLast;
	// if scan direction is forward, order spec computed should match
	// the index's sort and nulls order.
	if (scan_direction == EForwardScan)
	{
		// if sort direction of key is 0(ASC), choose MDID for less than
		// type and vice-versa
		mdid = (pmdindex->KeySortDirectionAt(key_position) == SORT_ASC)
				   ? colref->RetrieveType()->GetMdidForCmpType(IMDType::EcmptL)
				   : colref->RetrieveType()->GetMdidForCmpType(IMDType::EcmptG);

		// if nulls direction of key is 0, choose ENTLast and
		// vice-versa
		ent =
			(pmdindex->KeyNullsDirectionAt(key_position) == COrderSpec::EntLast)
				? COrderSpec::EntLast
				: COrderSpec::EntFirst;
	}
	// if scan direction is backward, order spec computed should be
	// commutative to index's sort and nulls order.
	else if (scan_direction == EBackwardScan)
	{
		// if sort order of key is 0(ASC), choose MDID for greater than
		// type and vice-versa
		mdid = (pmdindex->KeySortDirectionAt(key_position) == SORT_ASC)
				   ? colref->RetrieveType()->GetMdidForCmpType(IMDType::EcmptG)
				   : colref->RetrieveType()->GetMdidForCmpType(IMDType::EcmptL);

		// if nulls direction of key is 0, choose ENTFirst and
		// vice-versa
		ent =
			(pmdindex->KeyNullsDirectionAt(key_position) == COrderSpec::EntLast)
				? COrderSpec::EntFirst
				: COrderSpec::EntLast;
	}
	mdid->AddRef();
	(*order_spec)->Append(mdid, colref, ent);
}
// EOF
