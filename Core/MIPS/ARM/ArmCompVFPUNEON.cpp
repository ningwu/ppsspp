// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// NEON VFPU
// This is where we will create an alternate implementation of the VFPU emulation
// that uses NEON Q registers to cache pairs/tris/quads, and so on.
// Will require major extensions to the reg cache and other things.

// ARM NEON can only do pairs and quads, not tris and scalars.
// We can do scalars, though, for many operations if all the operands
// are below Q8 (D16, S32) using regular VFP instructions but really not sure
// if it's worth it.



#include <cmath>

#include "base/logging.h"
#include "math/math_util.h"

#include "Common/CPUDetect.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/Config.h"
#include "Core/Reporting.h"

#include "Core/MIPS/ARM/ArmJit.h"
#include "Core/MIPS/ARM/ArmRegCache.h"

// TODO: Somehow #ifdef away on ARMv5eabi, without breaking the linker.

#define CONDITIONAL_DISABLE ;
#define DISABLE { fpr.ReleaseSpillLocksAndDiscardTemps(); Comp_Generic(op); return; }

#define _RS MIPS_GET_RS(op)
#define _RT MIPS_GET_RT(op)
#define _RD MIPS_GET_RD(op)
#define _FS MIPS_GET_FS(op)
#define _FT MIPS_GET_FT(op)
#define _FD MIPS_GET_FD(op)
#define _SA MIPS_GET_SA(op)
#define _POS  ((op>> 6) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)
#define _IMM16 (signed short)(op & 0xFFFF)
#define _IMM26 (op & 0x03FFFFFF)


namespace MIPSComp {

static const float minus_one = -1.0f;
static const float one = 1.0f;
static const float zero = 0.0f;

ARMReg Jit::NEONMapPrefixST(int mipsReg, VectorSize sz, u32 prefix, int mapFlags) {
	static const float constantArray[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f/3.f, 0.25f, 1.f/6.f};
	static const float constantArrayNegated[8] = {-0.f, -1.f, -2.f, -0.5f, -3.f, -1.f/3.f, -0.25f, -1.f/6.f};

	// Applying prefixes in SIMD fashion will actually be a lot easier than the old style.
	if (prefix == 0xE4) {
		return fpr.QMapReg(mipsReg, sz, mapFlags);
	}
	int n = GetNumVectorElements(sz);

	int regnum[4];
	int abs[4];
	int negate[4];
	int constants[4];

	int abs_mask = (prefix >> 8) & 0xF;
	int negate_mask = (prefix >> 16) & 0xF;
	int constants_mask = (prefix >> 12) & 0xF;

	int full_mask = (1 << n) - 1;

	// Decode prefix to keep the rest readable
	int permuteMask = 0;
	for (int i = 0; i < n; i++) {
		permuteMask |= 3 << (i * 2);
		regnum[i] = (prefix >> (i*2)) & 3;
		abs[i]    = (prefix >> (8+i)) & 1;
		negate[i] = (prefix >> (16+i)) & 1;
		constants[i] = (prefix >> (12+i)) & 1;
	}

	bool anyPermute = (prefix & permuteMask) == (0xE4 & permuteMask);
	
	if (constants_mask == full_mask) {
		// It's all constants! Don't even bother mapping the input register,
		// just allocate a temp one.
		// If a single, this can sometimes be done cheaper. But meh.
		ARMReg ar = fpr.QAllocTemp();
		for (int i = 0; i < n; i++) {
			int constNum = regnum[i] + (abs[i] << 2);
			MOVP2R(R0, (negate[i] ? constantArrayNegated : constantArray) + constNum);
			VLD1_lane(F_32, ar, R0, i, true);
		}
		return ar;
	}

	// 1. Permute.
	// 2. Abs
	// If any constants:
	// 3. Replace values with constants
	// 4. Negate

	ARMReg inputAR = fpr.QMapReg(mipsReg, sz, mapFlags);
	ARMReg ar = fpr.QAllocTemp();
	if (!anyPermute) {
		VMOV(ar, inputAR);
		// No permutations!
	} else {
		bool allSame = true;
		for (int i = 1; i < n; i++) {
			if (regnum[0] == regnum[i])
				allSame = false;
		}
		if (allSame) {
			// Easy, someone is duplicating one value onto all the reg parts.
			// If this is happening and QMapReg must load, we can combine these two actions
			// into a VLD1_lane. TODO
			VDUP(F_32, ar, inputAR, regnum[0]);
		} else {
			// Can check for VSWP match?

			// TODO: Cannot do this permutation yet!
		}
	}

	// ABS
	// Two methods: If all lanes are "absoluted", it's easy.
	if (abs_mask == full_mask) {
		// TODO: elide the above VMOV when possible
		VABS(F_32, ar, ar);
	} else {
		// Partial ABS! TODO
	}
	
	if (negate_mask == full_mask) {
		// TODO: elide the above VMOV when possible
		VNEG(F_32, ar, ar);
	} else {
		// Partial negate! I guess we build sign bits in another register
		// and simply XOR.
	}

	// Insert constants where requested, and check negate!
	for (int i = 0; i < n; i++) {
		if (constants[i]) {
			int constNum = regnum[i] + (abs[i] << 2);
			MOVP2R(R0, (negate[i] ? constantArrayNegated : constantArray) + constNum);
			VLD1_lane(F_32, ar, R0, i, true);
		}
	}

	return ar;
}

inline ARMReg MatchSize(ARMReg x, ARMReg target) {
	// TODO
	return x;
}

Jit::DestARMReg Jit::NEONMapPrefixD(int vreg, VectorSize sz, int mapFlags) {
	// Inverted from the actual bits, easier to reason about 1 == write
	int writeMask = (~(js.prefixD >> 8)) & 0xF;

	DestARMReg dest;
	dest.sz = sz;
	if (writeMask == 0xF) {
		// No need to apply a write mask.
		// Let's not make things complicated.
		dest.rd = fpr.QMapReg(vreg, sz, mapFlags);
		dest.backingRd = dest.rd;
	} else {
		// Allocate a temporary register.
		dest.rd = fpr.QAllocTemp();
		dest.backingRd = fpr.QMapReg(vreg, sz, mapFlags & ~MAP_NOINIT);  // Force initialization of the backing reg.
	}
	return dest;
}

void Jit::NEONApplyPrefixD(DestARMReg dest) {
	// Apply clamps to dest.rd
	int n = GetNumVectorElements(dest.sz);

	int sat1_mask = 0;
	int sat3_mask = 0;
	int full_mask = 0;
	for (int i = 0; i < n; i++) {
		int sat = (js.prefixD >> (i * 2)) & 3;
		if (sat == 1)
			sat1_mask |= i << 1;
		if (sat == 3)
			sat3_mask |= i << 1;
		full_mask |= i << 1;
	}

	if (sat1_mask && sat3_mask) {
		// Why would anyone do this?
		ELOG("Can't have both sat[0-1] and sat[-1-1] at the same time");
	}

	if (sat1_mask) {
		if (sat1_mask != full_mask) {
			ELOG("Can't have partial sat1 mask");
		}
		MOVP2R(R0, &one);
		ARMReg temp = MatchSize(Q0, dest.rd);
		VLD1_all_lanes(F_32, temp, R0, true);
		MOVP2R(R0, &zero);
		VMIN(F_32, dest.rd, dest.rd, temp);
		VLD1_all_lanes(F_32, temp, R0, true);
		VMAX(F_32, dest.rd, dest.rd, temp);
	}

	if (sat3_mask && sat1_mask != full_mask) {
		if (sat1_mask != full_mask) {
			ELOG("Can't have partial sat3 mask");
		}
		MOVP2R(R0, &one);
		ARMReg temp = MatchSize(Q0, dest.rd);
		VLD1_all_lanes(F_32, temp, R0, true);
		MOVP2R(R0, &minus_one);
		VMIN(F_32, dest.rd, dest.rd, temp);
		VLD1_all_lanes(F_32, temp, R0, true);
		VMAX(F_32, dest.rd, dest.rd, temp);
	}

	// Check for mask operation
	if (dest.backingRd != dest.rd) {
		// This means that we need to apply the write mask, from rd to backingRd.
		// What a pain. We can at least shortcut easy cases like half the register.

		// TODO
		VMOV(dest.backingRd, dest.rd);
	}
}

void Jit::CompNEON_VecDo3(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix()) {
		DISABLE;
	}

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	ARMReg vs = NEONMapPrefixS(_VS, sz, 0);
	ARMReg vt = NEONMapPrefixT(_VT, sz, 0);
	DestARMReg vd = NEONMapPrefixD(_VD, sz, MAP_DIRTY | (vd == vs || vd == vt ? 0 : MAP_NOINIT));
	
	// TODO: Special case for scalar
	switch (op >> 26) {
	case 24: //VFPU0
		switch ((op >> 23) & 7) {
		case 0: VADD(F_32, vd, vs, vt); break; // vadd
		case 1: VSUB(F_32, vd, vs, vt); break; // vsub
		case 7: DISABLE; /* VDIV(F_32, vd, vs, vt); */  break; // vdiv  THERE IS NO NEON SIMD VDIV :(
		default:
			DISABLE;
		}
		break;
	case 25: //VFPU1
		switch ((op >> 23) & 7) {
		case 0: VMUL(F_32, vd, vs, vt); break;  // vmul
		default:
			DISABLE;
		}
		break;
	case 27: //VFPU3
		switch ((op >> 23) & 7)	{
		case 2: VMIN(F_32, vd, vs, vt); break;   // vmin
		case 3: VMAX(F_32, vd, vs, vt); break;   // vmax
		case 6:  // vsge
			DISABLE;  // pending testing
			break;
		case 7:  // vslt
			DISABLE;  // pending testing
			break;
		}
		break;

	default:
		DISABLE;
	}

	NEONApplyPrefixD(vd);

	fpr.ReleaseSpillLocksAndDiscardTemps();
	DISABLE;
}

void Jit::CompNEON_SV(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_SVQ(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	int imm = (signed short)(op&0xFFFC);
	int vt = (((op >> 16) & 0x1f)) | ((op&1) << 5);
	MIPSGPReg rs = _RS;
	bool doCheck = false;
	switch (op >> 26)
	{
	case 54: //lv.q
		{
			// CC might be set by slow path below, so load regs first.
			ARMReg ar = fpr.QMapReg(vt, V_Quad, MAP_DIRTY | MAP_NOINIT);
			if (gpr.IsImm(rs)) {
				u32 addr = (imm + gpr.GetImm(rs)) & 0x3FFFFFFF;
				gpr.SetRegImm(R0, addr + (u32)Memory::base);
			} else {
				gpr.MapReg(rs);
				if (g_Config.bFastMemory) {
					SetR0ToEffectiveAddress(rs, imm);
				} else {
					SetCCAndR0ForSafeAddress(rs, imm, R1);
					doCheck = true;
				}
				ADD(R0, R0, MEMBASEREG);
			}

			FixupBranch skip;
			if (doCheck) {
				skip = B_CC(CC_EQ);
			}

			VLD1(F_32, ar, R0, 2, ALIGN_128);

			if (doCheck) {
				SetJumpTarget(skip);
				SetCC(CC_AL);
			}
		}
		break;

	case 62: //sv.q
		{
			// CC might be set by slow path below, so load regs first.
			u8 vregs[4];
			ARMReg ar = fpr.QMapReg(vt, V_Quad, 0);

			if (gpr.IsImm(rs)) {
				u32 addr = (imm + gpr.GetImm(rs)) & 0x3FFFFFFF;
				gpr.SetRegImm(R0, addr + (u32)Memory::base);
			} else {
				gpr.MapReg(rs);
				if (g_Config.bFastMemory) {
					SetR0ToEffectiveAddress(rs, imm);
				} else {
					SetCCAndR0ForSafeAddress(rs, imm, R1);
					doCheck = true;
				}
				ADD(R0, R0, MEMBASEREG);
			}

			FixupBranch skip;
			if (doCheck) {
				skip = B_CC(CC_EQ);
			}

			VST1(F_32, ar, R0, 2, ALIGN_128);

			if (doCheck) {
				SetJumpTarget(skip);
				SetCC(CC_AL);
			}
		}
		break;

	default:
		DISABLE;
		break;
	}
	fpr.ReleaseSpillLocksAndDiscardTemps();
}

void Jit::CompNEON_VVectorInit(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VMatrixInit(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VDot(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VV2Op(MIPSOpcode op) {
	CONDITIONAL_DISABLE;
	if (js.HasUnknownPrefix()) {
		DISABLE;
	}

	// Pre-processing: Eliminate silly no-op VMOVs, common in Wipeout Pure
	if (((op >> 16) & 0x1f) == 0 && _VS == _VD && js.HasNoPrefix()) {
		return;
	}

	VectorSize sz = GetVecSize(op);
	int n = GetNumVectorElements(sz);

	ARMReg vs = NEONMapPrefixS(_VS, sz, 0);
	DestARMReg vd = NEONMapPrefixD(_VD, sz, MAP_DIRTY | (vd == vs ? 0 : MAP_NOINIT));

	switch ((op >> 16) & 0x1f) {
	case 0: // d[i] = s[i]; break; //vmov
		// Probably for swizzle.
		VMOV(vd, vs);
		break;
	case 1: // d[i] = fabsf(s[i]); break; //vabs
		VABS(F_32, vd, vs);
		break;
	case 2: // d[i] = -s[i]; break; //vneg
		VNEG(F_32, vd, vs);
		break;

	case 4: // if (s[i] < 0) d[i] = 0; else {if(s[i] > 1.0f) d[i] = 1.0f; else d[i] = s[i];} break;    // vsat0
		DISABLE;
		break;
	case 5: // if (s[i] < -1.0f) d[i] = -1.0f; else {if(s[i] > 1.0f) d[i] = 1.0f; else d[i] = s[i];} break;  // vsat1
		DISABLE;
		break;

	case 16: // d[i] = 1.0f / s[i]; break; //vrcp
		MOVP2R(R0, &one);
		VLD1_all_lanes(F_32, MatchSize(Q0, vs), R0, true);
		VDIV(vd, Q0, vs);
		break;
	case 17: // d[i] = 1.0f / sqrtf(s[i]); break; //vrsq
		DISABLE;
		break;
	case 18: // d[i] = sinf((float)M_PI_2 * s[i]); break; //vsin
		DISABLE;
		break;
	case 19: // d[i] = cosf((float)M_PI_2 * s[i]); break; //vcos
		DISABLE;
		break;
	case 20: // d[i] = powf(2.0f, s[i]); break; //vexp2
		DISABLE;
		break;
	case 21: // d[i] = logf(s[i])/log(2.0f); break; //vlog2
		DISABLE;
		break;
	case 22: // d[i] = sqrtf(s[i]); break; //vsqrt
		VSQRT(vd, vs);
		VABS(vd, vd);
		break;
	case 23: // d[i] = asinf(s[i] * (float)M_2_PI); break; //vasin
		DISABLE;
		break;
	case 24: // d[i] = -1.0f / s[i]; break; // vnrcp
		DISABLE;
		break;
	case 26: // d[i] = -sinf((float)M_PI_2 * s[i]); break; // vnsin
		DISABLE;
		break;
	case 28: // d[i] = 1.0f / expf(s[i] * (float)M_LOG2E); break; // vrexp2
		DISABLE;
		break;
	default:
		DISABLE;
		break;
	}

	NEONApplyPrefixD(vd);

	fpr.ReleaseSpillLocksAndDiscardTemps();
}

void Jit::CompNEON_Mftv(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vmtvc(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vmmov(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VScl(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vmmul(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vmscl(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vtfm(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VHdp(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VCrs(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VDet(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vi2x(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vx2i(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vf2i(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vi2f(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vh2f(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vcst(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vhoriz(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VRot(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VIdt(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vcmp(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vcmov(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Viim(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vfim(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_VCrossQuat(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vsgn(MIPSOpcode op) {
	DISABLE;
}

void Jit::CompNEON_Vocp(MIPSOpcode op) {
	DISABLE;
}

}
// namespace MIPSComp