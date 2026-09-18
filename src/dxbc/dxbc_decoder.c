// SPDX-License-Identifier: GPL-3.0-only

#include "dxbc/dxbc_decoder.h"

bool dxbc_opcode_is_known(uint32_t op) {
    /* Reserved holes in the D3D11.3 opcode enum must not be inferred from the
     * diagnostic label returned by dxbc_opcode_name(). */
    return op <= 234u && op != 107u && op != 112u && op != 209u &&
           op != 218u;
}

const char* dxbc_opcode_name(uint32_t op) {
    switch (op) {
        case 0: return "ADD";
        case 1: return "AND";
        case 2: return "BREAK";
        case 3: return "BREAKC";
        case 4: return "CALL";
        case 5: return "CALLC";
        case 6: return "CASE";
        case 7: return "CONTINUE";
        case 8: return "CONTINUEC";
        case 9: return "CUT";
        case 10: return "DEFAULT";
        case 11: return "DERIV_RTX";
        case 12: return "DERIV_RTY";
        case 13: return "DISCARD";
        case 14: return "DIV";
        case 15: return "DP2";
        case 16: return "DP3";
        case 17: return "DP4";
        case 18: return "ELSE";
        case 19: return "EMIT";
        case 20: return "EMITTHENCUT";
        case 21: return "ENDIF";
        case 22: return "ENDLOOP";
        case 23: return "ENDSWITCH";
        case 24: return "EQ";
        case 25: return "EXP";
        case 26: return "FRC";
        case 27: return "FTOI";
        case 28: return "FTOU";
        case 29: return "GE";
        case 30: return "IADD";
        case 31: return "IF";
        case 32: return "IEQ";
        case 33: return "IGE";
        case 34: return "ILT";
        case 35: return "IMAD";
        case 36: return "IMAX";
        case 37: return "IMIN";
        case 38: return "IMUL";
        case 39: return "INE";
        case 40: return "INEG";
        case 41: return "ISHL";
        case 42: return "ISHR";
        case 43: return "ITOF";
        case 44: return "LABEL";
        case 45: return "LD";
        case 46: return "LDMS";
        case 47: return "LOG";
        case 48: return "LOOP";
        case 49: return "LT";
        case 50: return "MAD";
        case 51: return "MIN";
        case 52: return "MAX";
        case 53: return "DCL_IMMEDIATECONSTANTBUFFER";
        case 54: return "MOV";
        case 55: return "MOVC";
        case 56: return "MUL";
        case 57: return "NE";
        case 58: return "NOP";
        case 59: return "NOT";
        case 60: return "OR";
        case 61: return "RESINFO";
        case 62: return "RET";
        case 63: return "RETC";
        case 64: return "ROUND_NE";
        case 65: return "ROUND_NI";
        case 66: return "ROUND_PI";
        case 67: return "ROUND_Z";
        case 68: return "RSQ";
        case 69: return "SAMPLE";
        case 70: return "SAMPLE_C";
        case 71: return "SAMPLE_C_LZ";
        case 72: return "SAMPLE_L";
        case 73: return "SAMPLE_D";
        case 74: return "SAMPLE_B";
        case 75: return "SQRT";
        case 76: return "SWITCH";
        case 77: return "SINCOS";
        case 78: return "UDIV";
        case 79: return "ULT";
        case 80: return "UGE";
        case 81: return "UMUL";
        case 82: return "UMAD";
        case 83: return "UMAX";
        case 84: return "UMIN";
        case 85: return "USHR";
        case 86: return "UTOF";
        case 87: return "XOR";
        case 88: return "DCL_RESOURCE";
        case 89: return "DCL_CONSTANTBUFFER";
        case 90: return "DCL_SAMPLER";
        case 91: return "DCL_INDEXRANGE";
        case 92: return "DCL_OUTPUTTOPOLOGY";
        case 93: return "DCL_INPUTPRIMITIVE";
        case 94: return "DCL_MAXOUT";
        case 95: return "DCL_INPUT";
        case 96: return "DCL_INPUT_SGV";
        case 97: return "DCL_INPUT_SIV";
        case 98: return "DCL_INPUT_PS";
        case 99: return "DCL_INPUT_PS_SGV";
        case 100: return "DCL_INPUT_PS_SIV";
        case 101: return "DCL_OUTPUT";
        case 102: return "DCL_OUTPUT_SGV";
        case 103: return "DCL_OUTPUT_SIV";
        case 104: return "DCL_TEMPS";
        case 105: return "DCL_INDEXABLETEMP";
        case 106: return "DCL_GLOBALFLAGS";
        case 108: return "LOD";
        case 109: return "GATHER4";
        case 110: return "SAMPLE_POS";
        case 111: return "SAMPLEINFO";
        case 113: return "HS_DECLS";
        case 114: return "HS_CONTROL_POINT_PHASE";
        case 115: return "HS_FORK_PHASE";
        case 116: return "HS_JOIN_PHASE";
        case 117: return "EMIT_STREAM";
        case 118: return "CUT_STREAM";
        case 119: return "EMITTHENCUT_STREAM";
        case 120: return "INTERFACE_CALL";
        case 121: return "BUFINFO";
        case 122: return "DERIV_RTX_COARSE";
        case 123: return "DERIV_RTX_FINE";
        case 124: return "DERIV_RTY_COARSE";
        case 125: return "DERIV_RTY_FINE";
        case 126: return "GATHER4_C";
        case 127: return "GATHER4_PO";
        case 128: return "GATHER4_PO_C";
        case 129: return "RCP";
        case 130: return "F32TOF16";
        case 131: return "F16TOF32";
        case 132: return "UADDC";
        case 133: return "USUBB";
        case 134: return "COUNTBITS";
        case 135: return "FIRSTBIT_HI";
        case 136: return "FIRSTBIT_LO";
        case 137: return "FIRSTBIT_SHI";
        case 138: return "UBFE";
        case 139: return "IBFE";
        case 140: return "BFI";
        case 141: return "BFREV";
        case 142: return "SWAPC";
        case 143: return "DCL_STREAM";
        case 144: return "DCL_FUNCTION_BODY";
        case 145: return "DCL_FUNCTION_TABLE";
        case 146: return "DCL_INTERFACE";
        case 147: return "DCL_INPUT_CONTROL_POINT_COUNT";
        case 148: return "DCL_OUTPUT_CONTROL_POINT_COUNT";
        case 149: return "DCL_TESSELLATOR_DOMAIN";
        case 150: return "DCL_TESSELLATOR_PARTITIONING";
        case 151: return "DCL_TESSELLATOR_OUTPUT_PRIMITIVE";
        case 152: return "DCL_HS_MAX_TESSFACTOR";
        case 153: return "DCL_HS_FORK_PHASE_INSTANCE_COUNT";
        case 154: return "DCL_HS_JOIN_PHASE_INSTANCE_COUNT";
        case 155: return "DCL_THREAD_GROUP";
        case 156: return "DCL_UAV_TYPED";
        case 157: return "DCL_UAV_RAW";
        case 158: return "DCL_UAV_STRUCTURED";
        case 159: return "DCL_TGSM_RAW";
        case 160: return "DCL_TGSM_STRUCTURED";
        case 161: return "DCL_RESOURCE_RAW";
        case 162: return "DCL_RESOURCE_STRUCTURED";
        case 163: return "LD_UAV_TYPED";
        case 164: return "STORE_UAV_TYPED";
        case 165: return "LD_RAW";
        case 166: return "STORE_RAW";
        case 167: return "LD_STRUCTURED";
        case 168: return "STORE_STRUCTURED";
        case 169: return "ATOMIC_AND";
        case 170: return "ATOMIC_OR";
        case 171: return "ATOMIC_XOR";
        case 172: return "ATOMIC_CMP_STORE";
        case 173: return "ATOMIC_IADD";
        case 174: return "ATOMIC_IMAX";
        case 175: return "ATOMIC_IMIN";
        case 176: return "ATOMIC_UMAX";
        case 177: return "ATOMIC_UMIN";
        case 178: return "IMM_ATOMIC_ALLOC";
        case 179: return "IMM_ATOMIC_CONSUME";
        case 180: return "IMM_ATOMIC_IADD";
        case 181: return "IMM_ATOMIC_AND";
        case 182: return "IMM_ATOMIC_OR";
        case 183: return "IMM_ATOMIC_XOR";
        case 184: return "IMM_ATOMIC_EXCH";
        case 185: return "IMM_ATOMIC_CMP_EXCH";
        case 186: return "IMM_ATOMIC_IMAX";
        case 187: return "IMM_ATOMIC_IMIN";
        case 188: return "IMM_ATOMIC_UMAX";
        case 189: return "IMM_ATOMIC_UMIN";
        case 190: return "SYNC";
        case 191: return "DADD";
        case 192: return "DMAX";
        case 193: return "DMIN";
        case 194: return "DMUL";
        case 195: return "DEQ";
        case 196: return "DGE";
        case 197: return "DLT";
        case 198: return "DNE";
        case 199: return "DMOV";
        case 200: return "DMOVC";
        case 201: return "DTOF";
        case 202: return "FTOD";
        case 203: return "EVAL_SNAPPED";
        case 204: return "EVAL_SAMPLE_INDEX";
        case 205: return "EVAL_CENTROID";
        case 206: return "DCL_GS_INSTANCE_COUNT";
        case 207: return "ABORT";
        case 208: return "DEBUG_BREAK";
        case 210: return "DDIV";
        case 211: return "DFMA";
        case 212: return "DRCP";
        case 213: return "MSAD";
        case 214: return "DTOI";
        case 215: return "DTOU";
        case 216: return "ITOD";
        case 217: return "UTOD";
        case 219: return "GATHER4_FEEDBACK";
        case 220: return "GATHER4_C_FEEDBACK";
        case 221: return "GATHER4_PO_FEEDBACK";
        case 222: return "GATHER4_PO_C_FEEDBACK";
        case 223: return "LD_FEEDBACK";
        case 224: return "LD_MS_FEEDBACK";
        case 225: return "LD_UAV_TYPED_FEEDBACK";
        case 226: return "LD_RAW_FEEDBACK";
        case 227: return "LD_STRUCTURED_FEEDBACK";
        case 228: return "SAMPLE_L_FEEDBACK";
        case 229: return "SAMPLE_C_LZ_FEEDBACK";
        case 230: return "SAMPLE_CLAMP_FEEDBACK";
        case 231: return "SAMPLE_B_CLAMP_FEEDBACK";
        case 232: return "SAMPLE_D_CLAMP_FEEDBACK";
        case 233: return "SAMPLE_C_CLAMP_FEEDBACK";
        case 234: return "CHECK_ACCESS_FULLY_MAPPED";
        default: return "UNKNOWN_OP";
    }
}

bool dxbc_opcode_is_declaration(uint32_t op) {
    /* These values come directly from D3D10_SB_OPCODE_TYPE.  CUSTOMDATA is
     * included because the parser projects immediate-constant-buffer custom
     * data as a declaration; every other range below is a DCL opcode. */
    return op == 53 || (op >= 88 && op <= 106) || op == 113 ||
           (op >= 143 && op <= 162) || op == 206;
}

const char* dxbc_sys_value_name(uint32_t sv) {
    switch (sv) {
        case 1: return "position";
        case 2: return "clip_distance";
        case 3: return "cull_distance";
        case 4: return "rendertarget_array_index";
        case 5: return "viewport_array_index";
        case 6: return "vertex_id";
        case 7: return "primitive_id";
        case 8: return "instance_id";
        case 9: return "is_front_face";
        case 10: return "sample_index";
        case 11: return "final_quad_edge_tessfactor";
        case 12: return "final_quad_inside_tessfactor";
        case 13: return "final_tri_edge_tessfactor";
        case 14: return "final_tri_inside_tessfactor";
        case 15: return "final_line_detail_tessfactor";
        case 16: return "final_line_density_tessfactor";
        case 23: return "barycentrics";
        case 24: return "shading_rate";
        case 25: return "cull_primitive";
        case 64: return "target";
        case 65: return "depth";
        case 66: return "coverage";
        case 67: return "depth_greater_equal";
        case 68: return "depth_less_equal";
        case 69: return "stencil_ref";
        case 70: return "inner_coverage";
        default: return "unknown_siv";
    }
}

const char* dxbc_interpolation_name(uint32_t mode) {
    switch (mode) {
        case 1: return " constant";
        case 2: return " linear";
        case 3: return " linear centroid";
        case 4: return " linear noperspective";
        case 5: return " linear noperspective centroid";
        case 6: return " linear sample";
        case 7: return " linear noperspective sample";
        default: return "";
    }
}

const char* dxbc_resource_dim_name(uint32_t dim) {
    switch (dim) {
        case 1: return "buffer";
        case 2: return "texture1d";
        case 3: return "texture2d";
        case 4: return "texture2dms";
        case 5: return "texture3d";
        case 6: return "texturecube";
        case 7: return "texture1darray";
        case 8: return "texture2darray";
        case 9: return "texture2dmsarray";
        case 10: return "texturecubearray";
        case 11: return "raw_buffer";
        case 12: return "structured_buffer";
        default: return "unknown_dim";
    }
}
