#include "XtensaOptimize.h"
#include "Bounds.h"
#include "ConciseCasts.h"
#include "CSE.h"
#include "ExprUsesVar.h"
#include "IREquality.h"
#include "IRMatch.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Lerp.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Expr.h"

namespace Halide {
namespace Internal {

using std::vector;

using namespace Halide::ConciseCasts;

struct Pattern {
    enum Flags {
        InterleaveResult = 1 << 0,  // After evaluating the pattern, interleave native vectors of the result.
        SwapOps01 = 1 << 1,         // Swap operands 0 and 1 prior to substitution.
        SwapOps12 = 1 << 2,         // Swap operands 1 and 2 prior to substitution.
        ExactLog2Op1 = 1 << 3,      // Replace operand 1 with its log base 2, if the log base 2 is exact.
        ExactLog2Op2 = 1 << 4,      // Save as above, but for operand 2.

        BeginExactLog2Op = 1,  // BeginExactLog2Op and EndExactLog2Op ensure that we check only op1 and op2
        EndExactLog2Op = 3,    // for ExactLog2Op

        DeinterleaveOp0 = 1 << 5,  // Prior to evaluating the pattern, deinterleave native vectors of operand 0.
        DeinterleaveOp1 = 1 << 6,  // Same as above, but for operand 1.
        DeinterleaveOp2 = 1 << 7,
        DeinterleaveOps = DeinterleaveOp0 | DeinterleaveOp1 | DeinterleaveOp2,

        BeginDeinterleaveOp = 0,  // BeginDeinterleaveOp and EndDeinterleaveOp ensure that we check only three
        EndDeinterleaveOp = 3,    // deinterleave Op0, 1 and 2.
        // Many patterns are instructions that widen only
        // operand 0, which need to both deinterleave operand 0, and then
        // re-interleave the result.
        ReinterleaveOp0 = InterleaveResult | DeinterleaveOp0,

        NarrowOp0 = 1 << 10,  // Replace operand 0 with its half-width equivalent.
        NarrowOp1 = 1 << 11,  // Same as above, but for operand 1.
        NarrowOp2 = 1 << 12,
        NarrowOp3 = 1 << 13,
        NarrowOps = NarrowOp0 | NarrowOp1 | NarrowOp2 | NarrowOp3,

        NarrowUnsignedOp0 = 1 << 15,  // Similar to the above, but narrow to an unsigned half width type.
        NarrowUnsignedOp1 = 1 << 16,
        NarrowUnsignedOp2 = 1 << 17,
        NarrowUnsignedOps = NarrowUnsignedOp0 | NarrowUnsignedOp1 | NarrowUnsignedOp2,
    };

    std::string intrin;  // Name of the intrinsic
    Expr pattern;   // The pattern to match against
    int flags;

    Pattern() = default;
    Pattern(const std::string &intrin, Expr p, int flags = 0)
        : intrin(intrin), pattern(std::move(p)), flags(flags) {
    }
};

Expr wild_u8 = Variable::make(UInt(8), "*");
Expr wild_u16 = Variable::make(UInt(16), "*");
Expr wild_u32 = Variable::make(UInt(32), "*");
Expr wild_u64 = Variable::make(UInt(64), "*");
Expr wild_i8 = Variable::make(Int(8), "*");
Expr wild_i16 = Variable::make(Int(16), "*");
Expr wild_i32 = Variable::make(Int(32), "*");
Expr wild_i64 = Variable::make(Int(64), "*");

Expr wild_u8x = Variable::make(Type(Type::UInt, 8, 0), "*");
Expr wild_u16x = Variable::make(Type(Type::UInt, 16, 0), "*");
Expr wild_u32x = Variable::make(Type(Type::UInt, 32, 0), "*");
Expr wild_u64x = Variable::make(Type(Type::UInt, 64, 0), "*");
Expr wild_i8x = Variable::make(Type(Type::Int, 8, 0), "*");
Expr wild_i16x = Variable::make(Type(Type::Int, 16, 0), "*");
Expr wild_i32x = Variable::make(Type(Type::Int, 32, 0), "*");
Expr wild_i64x = Variable::make(Type(Type::Int, 64, 0), "*");

// Broadcast to an unknown number of lanes, for making patterns.
Expr bc(Expr x) {
    return Broadcast::make(std::move(x), 0);
}

// Check if the matches satisfy the given pattern flags, and mutate the matches
// as specified by the flags.
bool process_match_flags(vector<Expr> &matches, int flags) {
    // The Pattern::Narrow*Op* flags are ordered such that the operand
    // corresponds to the bit (with operand 0 corresponding to the least
    // significant bit), so we can check for them all in a loop.
    for (size_t i = 0; i < matches.size(); i++) {
        Type t = matches[i].type();
        Type target_t = t.with_bits(t.bits() / 2);
        if (flags & (Pattern::NarrowOp0 << i)) {
            matches[i] = lossless_cast(target_t, matches[i]);
        } else if (flags & (Pattern::NarrowUnsignedOp0 << i)) {
            matches[i] = lossless_cast(target_t.with_code(Type::UInt), matches[i]);
        }
        if (!matches[i].defined()) return false;
    }

    for (size_t i = Pattern::BeginExactLog2Op; i < Pattern::EndExactLog2Op; i++) {
        // This flag is mainly to capture shifts. When the operand of a div or
        // mul is a power of 2, we can use a shift instead.
        if (flags & (Pattern::ExactLog2Op1 << (i - Pattern::BeginExactLog2Op))) {
            int pow;
            if (is_const_power_of_two_integer(matches[i], &pow)) {
                matches[i] = cast(matches[i].type().with_lanes(1), pow);
            } else {
                return false;
            }
        }
    }

    // for (size_t i = Pattern::BeginDeinterleaveOp; i < Pattern::EndDeinterleaveOp; i++) {
    //     if (flags & (Pattern::DeinterleaveOp0 << (i - Pattern::BeginDeinterleaveOp))) {
    //         internal_assert(matches[i].type().is_vector());
    //         matches[i] = native_deinterleave(matches[i]);
    //     }
    // }
    if (flags & Pattern::SwapOps01) {
        internal_assert(matches.size() >= 2);
        std::swap(matches[0], matches[1]);
    }
    if (flags & Pattern::SwapOps12) {
        internal_assert(matches.size() >= 3);
        std::swap(matches[1], matches[2]);
    }
    return true;
}

// Replace an expression with the one specified by a pattern.
Expr replace_pattern(Expr x, const vector<Expr> &matches, const Pattern &p) {
    x = Call::make(x.type(), p.intrin, matches, Call::PureExtern);
    // if (p.flags & Pattern::InterleaveResult) {
    //     // The pattern wants us to interleave the result.
    //     x = native_interleave(x);
    // }
    return x;
}
// Attempt to apply one of the patterns to x. If a match is
// successful, the expression is replaced with a call using the
// matched operands. Prior to substitution, the matches are mutated
// with op_mutator.
Expr apply_patterns(Expr x, const vector<Pattern> &patterns, IRMutator *op_mutator) {
    debug(3) << "apply_patterns " << x << "\n";
    vector<Expr> matches;
    for (const Pattern &p : patterns) {
        if (expr_match(p.pattern, x, matches)) {
            debug(3) << "matched " << p.pattern << "\n";
            debug(3) << "to " << x << "\n";
            debug(3) << "matches:\n";
            for (Expr i : matches) {
                debug(3) << i << "\n";
            }

            if (!process_match_flags(matches, p.flags)) {
                continue;
            }

            // // Don't apply pattern if it involves an interleave,
            // // and is not a multiple of two vectors.
            // // See https://github.com/halide/Halide/issues/1582
            // if ((p.flags & Pattern::InterleaveResult) && !is_double_vector(x, target)) {
            //     continue;
            // }
            // Mutate the operands with the given mutator.
            for (Expr &op : matches) {
                op = op_mutator->mutate(op);
            }

            x = replace_pattern(x, matches, p);
            debug(3) << "rewrote to: " << x << "\n";
            return x;
        }
    }
    return x;
}

template<typename T>
Expr apply_commutative_patterns(const T *op, const vector<Pattern> &patterns, IRMutator *mutator) {
    Expr ret = apply_patterns(op, patterns, mutator);
    if (!ret.same_as(op)) return ret;

    // Try commuting the op
    Expr commuted = T::make(op->b, op->a);
    ret = apply_patterns(commuted, patterns, mutator);
    if (!ret.same_as(commuted)) return ret;

    return op;
}

class MatchXtensaPatterns : public IRMutator {
private:
    using IRMutator::visit;

    static Expr halide_xtensa_widen_mul_i32(Expr v0, Expr v1) {
        Expr call = Call::make(wild_i32x.type(), "halide_xtensa_widen_mul_i32", {std::move(v0), std::move(v1)}, Call::PureExtern);
        return call;
    }

    static Expr halide_xtensa_widen_mul_u32(Expr v0, Expr v1) {
        Expr call = Call::make(wild_u32x.type(), "halide_xtensa_widen_mul_u32", {std::move(v0), std::move(v1)}, Call::PureExtern);
        return call;
    }

    static Expr halide_xtensa_widen_mul_add1(Expr v0, Expr v1, Expr v2) {
        Expr call = Call::make(wild_i32x.type(), "halide_xtensa_widen_mul_add1", {std::move(v0), std::move(v1), std::move(v2)}, Call::PureExtern);
        return call;
    }

    static Expr halide_xtensa_widen_mul_add2(Expr v0, Expr v1, Expr v2, Expr v3) {
        Expr call = Call::make(wild_i32x.type(), "halide_xtensa_widen_mul_add2", {std::move(v0), std::move(v1), std::move(v2), std::move(v3)}, Call::PureExtern);
        return call;
    }

    Expr visit(const Add *op) override {
        if (op->type.is_vector()) {
            static const std::vector<Pattern> adds = {
                // {"halide_xtensa_widen_mul_add_mul_u32", (halide_xtensa_widen_mul_u32(wild_u16x, wild_u16x) / 2)
                //                                            + (halide_xtensa_widen_mul_u32(wild_u16x, wild_u16x) / 2)},

                // {"halide_xtensa_widen_mul_add1", i32(wild_i16x) + halide_xtensa_widen_mul_i32(wild_i16x, wild_i16x)},
                // {"halide_xtensa_widen_mul_add2", i32(wild_i16x) + halide_xtensa_widen_mul_add1(wild_i16x, wild_i16x, wild_i16)},
                // {"halide_xtensa_widen_mul_add3", i32(wild_i16x) + halide_xtensa_widen_mul_add2(wild_i16x, wild_i16x, wild_i16x, wild_i16)},

                // Widening addition
                // {"halide_xtensa_widen_add_u32", wild_u32x + wild_u32x, Pattern::NarrowOp1},
                // {"halide_xtensa_widen_add_i32", wild_i32x + wild_i32x, Pattern::NarrowOp1},
                // {"halide_xtensa_widen_mul_add_i32", wild_i32x + wild_i32x * bc(wild_i32), Pattern::NarrowOps },
                // {"halide_xtensa_widen_mul_add_i32", wild_i32x + bc(wild_i32) * wild_i32x, Pattern::NarrowOps | Pattern::SwapOps12},

                // {"halide_xtensa_widen_mul_add_u32", wild_u32x + wild_u32x * bc(wild_u32), Pattern::NarrowOps },
                // {"halide_xtensa_widen_mul_add_u32", wild_u32x + bc(wild_u32) * wild_u32x, Pattern::NarrowOps | Pattern::SwapOps12},
            };

            Expr new_expr = apply_commutative_patterns(op, adds, this);
            if (!new_expr.same_as(op)) {
                return new_expr;
            }
        }

        return IRMutator::visit(op);
    }

    Expr visit(const Mul *op) override {
        if (op->type.is_vector()) {
            static const std::vector<Pattern> scalar_muls = {
            };

            static const std::vector<Pattern> muls = {
                // Widening multiplication
                {"halide_xtensa_widen_mul_i32", wild_i32x * bc(wild_i32), Pattern::NarrowOps},

                {"halide_xtensa_widen_mul_u16", wild_u16x * wild_u16x, Pattern::NarrowOps},
                {"halide_xtensa_widen_mul_u32", wild_u32x * wild_u32x, Pattern::NarrowOps},
                {"halide_xtensa_widen_mul_i16", wild_i16x * wild_i16x, Pattern::NarrowOps},
                {"halide_xtensa_widen_mul_i32", wild_i32x * wild_i32x, Pattern::NarrowOps},
            };

            Expr new_expr = apply_commutative_patterns(op, scalar_muls, this);
            if (!new_expr.same_as(op)) {
                return new_expr;
            }

            new_expr = apply_commutative_patterns(op, muls, this);
            if (!new_expr.same_as(op)) {
                return new_expr;
            }
        }

        return IRMutator::visit(op);
    }

//     Expr visit(const Select* op) {
//         if (op->type.is_vector()) {
//           static const vector<Pattern> selects = {
//             // {"halide_xtensa_amazing_select", select(0 < (((u32(wild_u16x) * u32(wild_u16x)) / 2) + ((u32(wild_u16x) * u32(wild_u16x)) / 2)), bc(wild_i16) - i16(count_leading_zeros(((u32(wild_u16x) * u32(wild_u16x)) / 2) + ((u32(wild_u16x) * u32(wild_u16x)) / 2))), bc(wild_i16))},
//             // {"halide_xtensa_funny_select", select(0 < (i32(wild_i16x) * i32(wild_i16x)), bc(wild_i16) - i16(count_leading_zeros((i32(wild_i16x) * i32(wild_i16x)))), bc(wild_i16))},
//           };
//           vector<Expr> matches;
//           for (const auto& p: selects) {
//             if (expr_match(p.pattern, op, matches)) {
//               debug(0) << "Matched select !! " << p.intrin << matches.size() << "\n";

//               for (Expr &m : matches) {
//                   m = mutate(m);
//               }

//               debug(0) << matches[0].same_as(matches[1]) << " " << matches[3].same_as(matches[4]) << "\n";
//               return Call::make(op->type, p.intrin,
//                                 //{matches[0], matches[2], matches[5]},
//                                 matches,
//                     Call::PureExtern);
//             }

//           }
//         }
//         return IRMutator::visit(op);
//     }

//     Expr visit(const LT *op) override {
//         static const vector<Pattern> lts = {
//           // {"halide_xtensa_nice_lt", 0 < ((u32(wild_u16x) * u32(wild_u16x)) / 2)},
//         };

//         if (op->type.is_vector()) {
//             Expr lt = op;

//             std::vector<Expr> matches;

//             Expr new_expr = apply_patterns(lt, lts, this);
//             if (!new_expr.same_as(lt)) {
//                 return new_expr;
//             }
//         }

//         return IRMutator::visit(op);
//     }

    Expr visit(const Cast *op) override {
        static const std::vector<Pattern> casts = {
            // Averaging
            {"halide_xtensa_avg_u16", u16((wild_u32x + wild_u32x) / 2), Pattern::NarrowOps},
            {"halide_xtensa_avg_i16", i16((wild_i32x + wild_i32x) / 2), Pattern::NarrowOps},

            {"halide_xtensa_avg_round_u16", u16((wild_u32x + wild_u32x + 1) / 2), Pattern::NarrowOps},
            {"halide_xtensa_avg_round_i16", i16((wild_i32x + wild_i32x + 1) / 2), Pattern::NarrowOps},

            // Saturating add/subtract
            {"halide_xtensa_sat_add_i16", i16_sat(wild_i32x + wild_i32x), Pattern::NarrowOps},
            {"halide_xtensa_sat_add_i32", i32_sat(wild_i64x + wild_i64x), Pattern::NarrowOps},
            {"halide_xtensa_sat_sub_i16", i16_sat(wild_i32x - wild_i32x), Pattern::NarrowOps},

            // Narrowing with shifting.
            {"halide_xtensa_narrow_with_shift_i16", i16(wild_i32x >> wild_i32)},
            {"halide_xtensa_narrow_with_shift_i16", i16(wild_i32x / wild_i32), Pattern::ExactLog2Op1},

            {"halide_xtensa_narrow_clz_i16", i16(count_leading_zeros(wild_u32x))},
            {"halide_xtensa_narrow_clz_i16", i16(count_leading_zeros(wild_i32x))},
        };
        if (op->type.is_vector()) {
            Expr cast = op;

            std::vector<Expr> matches;

            Expr new_expr = apply_patterns(cast, casts, this);
            if (!new_expr.same_as(cast)) {
                return new_expr;
            }
        }

        return IRMutator::visit(op);
    }

    Expr visit(const Shuffle* op) override {
      if (op->is_interleave() && op->type.is_int() && (op->type.bits() == 16) && (op->type.lanes() == 64)) {
          debug(0) << "Recognized supported interleave\n";
          return Call::make(op->type, "halide_xtensa_interleave_i16",
                            {mutate(op->vectors[0]), mutate(op->vectors[1])},
                            Call::PureExtern);
      } else {
          return IRMutator::visit(op);
      }
    }

    Expr visit(const Call *op) override {
        // if (op->is_intrinsic(Call::lerp) && op->type.is_int() && (op->type.bits() == 16) && (op->type.lanes() == 32)) {
        //   internal_assert(op->args.size() == 3);
        //   // debug(0) << "Lerp - " << op->args[0] << " " << op->args[1] << " " << op->args[2] << "\n";
        //   // debug(0) << "Lerp types - " << op->args[0].type() << " " << op->args[1].type() << " " << op->args[2].type() << "\n";
        //   Expr weight = mutate(op->args[2]);
        //   const Broadcast* maybe_bc = weight.as<Broadcast>();
        //   if (maybe_bc) {
        //     weight = maybe_bc->value;
        //   }
        //   return Call::make(op->type, "halide_xtensa_lerp_i16",
        //                     {mutate(op->args[0]), mutate(op->args[1]), weight},
        //                     Call::PureExtern);
        // } else
        if (op->is_intrinsic(Call::absd) && op->type.is_vector()
                   && op->type.is_uint() && (op->type.bits() == 16)) {
            // debug(0) << "Found absd " << op->type.is_vector() << " " << op->type.is_uint() << " " << (op->type.bits() == 16) << "\n";
            internal_assert(op->args.size() == 2);
            return Call::make(op->type, "halide_xtensa_absd_i16",
                              {mutate(op->args[0]), mutate(op->args[1])},
                              Call::PureExtern);
        }

        return IRMutator::visit(op);
    }

    int loop_depth_ = 0;

    Stmt visit(const For* op) override {
      loop_depth_++;
      Stmt body = IRMutator::visit(op);
      loop_depth_--;
      return body;
    }

    Stmt visit(const LetStmt *op) override {
      if (loop_depth_ < 1) {
        return IRMutator::visit(op);
      }

      if (op->value.type().is_handle()) {
          return IRMutator::visit(op);
      }

      Stmt body = op->body;
      body = substitute(op->name, op->value, body);
      return mutate(body);
    }

public:
    MatchXtensaPatterns() {}
};

// Find an upper bound of bounds.max - bounds.min.
Expr span_of_bounds(const Interval &bounds) {
    internal_assert(bounds.is_bounded());

    const Min *min_min = bounds.min.as<Min>();
    const Max *min_max = bounds.min.as<Max>();
    const Min *max_min = bounds.max.as<Min>();
    const Max *max_max = bounds.max.as<Max>();
    const Add *min_add = bounds.min.as<Add>();
    const Add *max_add = bounds.max.as<Add>();
    const Sub *min_sub = bounds.min.as<Sub>();
    const Sub *max_sub = bounds.max.as<Sub>();

    if (min_min && max_min && equal(min_min->b, max_min->b)) {
        return span_of_bounds({min_min->a, max_min->a});
    } else if (min_max && max_max && equal(min_max->b, max_max->b)) {
        return span_of_bounds({min_max->a, max_max->a});
    } else if (min_add && max_add && equal(min_add->b, max_add->b)) {
        return span_of_bounds({min_add->a, max_add->a});
    } else if (min_sub && max_sub && equal(min_sub->b, max_sub->b)) {
        return span_of_bounds({min_sub->a, max_sub->a});
    } else {
        return bounds.max - bounds.min;
    }
}

// Replace indirect loads with dynamic_shuffle intrinsics where
// possible.
class OptimizeShuffles : public IRMutator {
    int lut_alignment;
    Scope<Interval> bounds;
    std::vector<std::pair<std::string, Expr>> lets;

    using IRMutator::visit;

    template<typename NodeType, typename T>
    NodeType visit_let(const T *op) {
        // We only care about vector lets.
        if (op->value.type().is_vector()) {
            bounds.push(op->name, bounds_of_expr_in_scope(op->value, bounds));
        }
        NodeType node = IRMutator::visit(op);
        if (op->value.type().is_vector()) {
            bounds.pop(op->name);
        }
        return node;
    }

    Expr visit(const Let *op) override {
        lets.emplace_back(op->name, op->value);
        Expr expr = visit_let<Expr>(op);
        lets.pop_back();
        return expr;
    }
    Stmt visit(const LetStmt *op) override {
        return visit_let<Stmt>(op);
    }

    Expr visit(const Load *op) override {
        if (!is_one(op->predicate)) {
            // TODO(psuriana): We shouldn't mess with predicated load for now.
            return IRMutator::visit(op);
        }
        if (!op->type.is_vector() || op->index.as<Ramp>()) {
            // Don't handle scalar or simple vector loads.
            return IRMutator::visit(op);
        }

        Expr index = mutate(op->index);
        Interval unaligned_index_bounds = bounds_of_expr_in_scope(index, bounds);
        if (unaligned_index_bounds.is_bounded()) {
            // We want to try both the unaligned and aligned
            // bounds. The unaligned bounds might fit in 64 elements,
            // while the aligned bounds do not.
            int align = lut_alignment / op->type.bytes();
            Interval aligned_index_bounds = {
                (unaligned_index_bounds.min / align) * align,
                ((unaligned_index_bounds.max + align) / align) * align - 1};
            ModulusRemainder alignment(align, 0);

            for (Interval index_bounds : {aligned_index_bounds, unaligned_index_bounds}) {
                Expr index_span = span_of_bounds(index_bounds);
                index_span = common_subexpression_elimination(index_span);
                index_span = simplify(index_span);

                if (can_prove(index_span < 64)) {
                    // This is a lookup within an up to 64 element array. We
                    // can use dynamic_shuffle for this.
                    // TODO(vksnk): original code doesn't align/pad here, why?
                    int const_extent = as_const_int(index_span) ? (((*as_const_int(index_span) + align) / align) * align) : 64;
                    Expr base = simplify(index_bounds.min);

                    debug(0) << "const_extent - " << const_extent << "\n";
                    // Load all of the possible indices loaded from the
                    // LUT. Note that for clamped ramps, this loads up to 1
                    // vector past the max. CodeGen_Hexagon::allocation_padding
                    // returns a native vector size to account for this.
                    Expr lut = Load::make(op->type.with_lanes(const_extent), op->name,
                                          Ramp::make(base, 1, const_extent),
                                          op->image, op->param, const_true(const_extent), alignment);

                    // We know the size of the LUT is not more than 64, so we
                    // can safely cast the index to 16 bit, which
                    // dynamic_shuffle requires.
                    index = simplify(cast(Int(16).with_lanes(op->type.lanes()), index - base));
                    return Call::make(op->type, "halide_xtensa_dynamic_shuffle", {lut, index, 0, const_extent - 1}, Call::PureExtern);
                }
                // Only the first iteration of this loop is aligned.
                alignment = ModulusRemainder();
            }
        }
        if (!index.same_as(op->index)) {
            return Load::make(op->type, op->name, index, op->image, op->param, op->predicate, op->alignment);
        } else {
            return op;
        }
    }

public:
    OptimizeShuffles(int lut_alignment)
        : lut_alignment(lut_alignment) {
    }
};

// class CollectSimilarOps : public IRVisitor {
//  public:
//   std::vector<Expr>* leaves;
//   CollectSimilarOps(vector<Expr>* l) : leaves(l) {}

//  private:
//   using IRVisitor::visit;

//   void visit(const Add* op) {
//     debug(0) << "Found add - \n";// << op->a << " " << op->b << "\n";
//     if (op->a.node_type() == IRNodeType::Add) {
//       op->a->accept(this);
//     } else {
//       leaves->push_back(op->a);
//     }

//     if (op->b.node_type() == IRNodeType::Add) {
//       op->b->accept(this);
//     } else {
//       leaves->push_back(op->b);
//     }

//   }
// };

// bool try_to_add_expr(int v, const vector<vector<int>>& g,
//                        vector<bool>& visited, vector<int>& match) {
//   visited[v] = true;
//   for (int j = 0; j < g[v].size(); j++) {
//     int t = g[v][j];
//     debug(0) << v << " " << t << "\n";
//     if ((match[t] == -1) || (!visited[match[t]] && try_to_add_expr(match[t], g, visited, match))) {
//       match[t] = v;
//       return true;
//     }
//   }
//   return false;
// }

// bool commutative_expr_match(const Expr &pattern, const Expr &expr, vector<Expr> &matches) {
//   // matches.clear();
//   // if (!pattern.defined() && !expr.defined()) return true;
//   // if (!pattern.defined() || !expr.defined()) return false;

//   if (const Add *add = pattern.as<Add>()) {
//   } else if (const Cast *cast = pattern.as<Cast>()) {

//   } else {
//     return expr_match(pattern, expr, matches);
//   }

//   return true;
// }

Stmt match_xtensa_patterns(Stmt s) {
//     Expr test_pattern1 = wild_i16x + ((wild_i16x + wild_i16x) * wild_i16x
//                         + i16_sat(wild_i16x * wild_i32x) + wild_i16x * bc(wild_i16));
//     Expr test_pattern2 = wild_i16x * bc(wild_i16) +  wild_i16x
//                         + i16_sat(wild_i16x * wild_i32x) + (wild_i16x + wild_i16x) * wild_i16x;
//     std::vector<Expr> leaves1;
//     std::vector<Expr> leaves2;
//     {
//       debug(0) << "Looking for ads\n";
//       CollectSimilarOps collect_ops(&leaves1);
//       test_pattern1.accept(&collect_ops);
//       for(const auto& l: leaves1) {
//         debug(0) << "Found: " << l << "\n";
//       }
//     }

//     {
//       debug(0) << "Looking for adds\n";
//       CollectSimilarOps collect_ops(&leaves2);
//       test_pattern2.accept(&collect_ops);
//       for(const auto& l: leaves2) {
//         debug(0) << "Found: " << l << "\n";
//       }
//     }

//     int n = leaves1.size();
//     int k = leaves2.size();
//     vector<vector<int>> g(n);
//     for (int i = 0; i < n; i++) {
//       for (int j = 0; j < k; j++) {
//         std::vector<Expr> matches;
//         bool is_matching = expr_match(leaves1[i], leaves2[j], matches);
//         if (is_matching) {
//           g[i].push_back(j);
//         }
//         debug(0) << is_matching << " ";
//       }
//       debug(0) << "\n";
//     }

//     std::vector<int> match(n, -1);
//     for (int v = 0; v < n; v++) {
//       std::vector<bool> visited(n);
//       debug(0) << "Starting - " << v << "\n";
//       try_to_add_expr(v, g, visited, match);
//     }

//     for (int v = 0; v < n; v++) {
//       debug(0) << match[v] << " -> " << v << "\n";
//     }

    s = OptimizeShuffles(64).mutate(s);
    // debug(0) << s << "\n";
    for (int ix = 0; ix < 10; ix++) {
      s = MatchXtensaPatterns().mutate(s);
    }

    s = simplify(common_subexpression_elimination(s));
    return s;
}

}  // namespace Internal
}  // namespace Halide