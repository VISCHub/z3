/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    theory_pb.cpp

Abstract:

    Pseudo-Boolean theory plugin.

Author:

    Nikolaj Bjorner (nbjorner) 2013-11-05

Notes:


--*/

#include "theory_pb.h"
#include "smt_context.h"
#include "ast_pp.h"
#include "sorting_network.h"
#include "uint_set.h"
#include "smt_model_generator.h"
#include "pb_rewriter_def.h"

namespace smt {

    // parametric sorting network
    // Described in Abio et.al. CP 2013.
    class psort_nw {
        class vc {
            unsigned v; // number of vertices
            unsigned c; // number of clauses
            static const unsigned lambda = 5;
        public:
            vc(unsigned v, unsigned c):v(v), c(c) {}

            bool operator<(vc const& other) const {
                return to_int() < other.to_int();
            }
            vc operator+(vc const& other) const {
                return vc(v + other.v, c + other.c);
            }
            unsigned to_int() const {
                return lambda*v + c;
            }
            vc operator*(unsigned n) const {
                return vc(n*v, n*c);
            }
        };

        static vc min(vc const& v1, vc const& v2) {
            return (v1.to_int() < v2.to_int())?v1:v2;
        }


        enum cmp_t { LE, GE, EQ, GE_FULL, LE_FULL };
        context&     ctx;
        cmp_t        m_t;

        // for testing
        static const bool m_disable_dcard    = false;
        static const bool m_disable_dsorting = false;
        static const bool m_disable_dsmerge  = false;
        static const bool m_force_dcard      = false;
        static const bool m_force_dsorting   = false;
        static const bool m_force_dsmerge    = false;

    public:
        struct stats {
            unsigned m_num_compiled_vars;
            unsigned m_num_compiled_clauses;
            void reset() { memset(this, 0, sizeof(*this)); }
            stats() { reset(); }
        };
        stats        m_stats;

        psort_nw(context& c): ctx(c) {}

        literal ge(bool full, unsigned k, unsigned n, literal const* xs) {
            if (k > n) {
                return false_literal;
            }
            if (k == 0) {
                return true_literal;
            }
            SASSERT(0 < k && k <= n);
            literal_vector in, out;
            if (dualize(k, n, xs, in)) {
                return le(full, k, in.size(), in.c_ptr());
            }
            else {
                SASSERT(2*k <= n);
                m_t = full?GE_FULL:GE;
                card(k, n, xs, out);                
                return out[k-1]; 
            }
        }
        
        literal le(bool full, unsigned k, unsigned n, literal const* xs) {
            if (k >= n) {
                return true_literal;
            }
            SASSERT(k < n);
            literal_vector in, out;
            if (dualize(k, n, xs, in)) {
                return ge(full, k, n, in.c_ptr());
            }
            else {
                SASSERT(2*k <= n);
                m_t = full?LE_FULL:LE;
                card(k + 1, n, xs, out);
                return ~out[k]; 
            }
        }

        literal eq(unsigned k, unsigned n, literal const* xs) {
            if (k > n) {
                return false_literal;
            }
            SASSERT(k <= n);
            literal_vector in, out;
            if (dualize(k, n, xs, in)) {
                return eq(k, n, in.c_ptr());
            }
            else {
                SASSERT(2*k < n);
                m_t = EQ;
                card(k+1, n, xs, out);
                SASSERT(out.size() >= k+1);
                return out[k-1]; // & ~out[m] TBD
            }
        }

        
    private:

        std::ostream& pp(std::ostream& out, unsigned n, literal const* lits) {
            for (unsigned i = 0; i < n; ++i) out << lits[i] << " ";
            return out;
        }

        std::ostream& pp(std::ostream& out, literal_vector const& lits) {
            for (unsigned i = 0; i < lits.size(); ++i) out << lits[i] << " ";
            return out;
        }

        std::ostream& ppv(std::ostream& out, unsigned n, literal const* lits) {
            for (unsigned i = 0; i < n; ++i) {
                expr_ref tmp(ctx.get_manager());
                ctx.literal2expr(lits[i], tmp);
                out << tmp << " ";
            }
            return out;
        }

        std::ostream& ppv(std::ostream& out, literal_vector const& lits) {
            for (unsigned i = 0; i < lits.size(); ++i) {
                expr_ref tmp(ctx.get_manager());
                ctx.literal2expr(lits[i], tmp);
                out << tmp << " ";
            }
            return out;
        }

        // 0 <= k <= N
        //     SUM x_i >= k 
        // <=>
        //     SUM ~x_i <= N - k
        // suppose k > N/2, then it is better to solve dual.

        bool dualize(unsigned& k, unsigned N, literal const* xs, literal_vector& in) {
            SASSERT(0 <= k && k <= N);
            if (2*k <= N) {
                return false;
            }
            k = N - k;
            for (unsigned i = 0; i < N; ++i) {
                in.push_back(~xs[i]);
            }
            TRACE("pb", 
                  pp(tout << N << ": ", in);
                  tout << " ~ " << k << "\n";);
            return true;
        }
        

        bool even(unsigned n) const { return (0 == (n & 0x1)); }
        bool odd(unsigned n) const { return !even(n); }
        unsigned ceil2(unsigned n) const { return n/2 + odd(n); }
        unsigned floor2(unsigned n) const { return n/2; }
        unsigned power2(unsigned n) const { SASSERT(n < 10); return 1 << n; }

        literal max(literal a, literal b) {
            if (a == b) return a;
            m_stats.m_num_compiled_vars++;
            ast_manager& m = ctx.get_manager();
            expr_ref t1(m), t2(m), t3(m);
            ctx.literal2expr(a, t1);
            ctx.literal2expr(b, t2);
            t3 = m.mk_or(t1, t2);
            bool_var v = ctx.b_internalized(t3)?ctx.get_bool_var(t3):ctx.mk_bool_var(t3);
            return literal(v);
        }

        literal min(literal a, literal b) {
            if (a == b) return a;
            m_stats.m_num_compiled_vars++;
            ast_manager& m = ctx.get_manager();
            expr_ref t1(m), t2(m), t3(m);
            ctx.literal2expr(a, t1);
            ctx.literal2expr(b, t2);
            t3 = m.mk_and(t1, t2);
            bool_var v = ctx.b_internalized(t3)?ctx.get_bool_var(t3):ctx.mk_bool_var(t3);
            return literal(v);
        }

        literal fresh() {
            m_stats.m_num_compiled_vars++;
            ast_manager& m = ctx.get_manager();
            app_ref y(m);
            y = m.mk_fresh_const("y", m.mk_bool_sort());
            return literal(ctx.mk_bool_var(y));
        }
        void add_clause(literal l1, literal l2, literal l3) {
            literal lits[3] = { l1, l2, l3 };
            add_clause(3, lits);
        }
        void add_clause(literal l1, literal l2) {
            literal lits[2] = { l1, l2 };
            add_clause(2, lits);
        }
        void add_clause(unsigned n, literal const* ls) {
            m_stats.m_num_compiled_clauses++;
            literal_vector tmp(n, ls);
            TRACE("pb", pp(tout, n, ls) << "\n";);
            ctx.mk_clause(n, tmp.c_ptr(), 0, CLS_AUX, 0);
        }

        // y1 <= max(x1,x2)
        // y2 <= min(x1,x2)
        void cmp_ge(literal x1, literal x2, literal y1, literal y2) {
            add_clause(~y2, x1);
            add_clause(~y2, x2);
            add_clause(~y1, x1, x2);            
        }

        // max(x1,x2) <= y1
        // min(x1,x2) <= y2
        void cmp_le(literal x1, literal x2, literal y1, literal y2) {
            add_clause(~x1, y1);
            add_clause(~x2, y1);
            add_clause(~x1, ~x2, y2);
        }

        void cmp_eq(literal x1, literal x2, literal y1, literal y2) {
            cmp_ge(x1, x2, y1, y2);
            cmp_le(x1, x2, y1, y2);
        }

        void cmp(literal x1, literal x2, literal y1, literal y2) {
            switch(m_t) {
            case LE: cmp_le(x1, x2, y1, y2); break;
            case GE: cmp_ge(x1, x2, y1, y2); break;
            case EQ: cmp_eq(x1, x2, y1, y2); break;
            }
        }
        vc vc_cmp() {
            return vc(2, (m_t==EQ)?6:3);
        }

        void card(unsigned k, unsigned n, literal const* xs, literal_vector& out) {
            TRACE("pb", tout << "card k:" << k << " n: " << n << "\n";);
            if (n <= k) {
                sorting(n, xs, out);
            }
            else if (use_dcard(k, n)) {
                dsorting(k, n, xs, out);
            }
            else {
                literal_vector out1, out2;
                unsigned l = n/2; // TBD
                card(k, l, xs, out1);
                card(k, n-l, xs + l, out2);
                smerge(k, out1.size(), out1.c_ptr(), out2.size(), out2.c_ptr(), out);
            }
            TRACE("pb", tout << "card k:" << k << " n: " << n << "\n";
                  pp(tout << "in:", n, xs) << "\n";
                  pp(tout << "out:", out) << "\n";);

        }        
        vc vc_card(unsigned k, unsigned n) {
            if (n <= k) {
                return vc_sorting(n);
            }
            else if (use_dcard(k, n)) {
                return vc_dsorting(k, n);
            }
            else {
                return vc_card_rec(k, n);
            }
        }
        vc vc_card_rec(unsigned k, unsigned n) {
            unsigned l = n/2;
            return vc_card(k, l) + vc_card(k, n-l) + vc_smerge(k, l, n-l);
        }
        bool use_dcard(unsigned k, unsigned n) {
            return m_force_dcard || (!m_disable_dcard && n < 10 && vc_dsorting(k, n) < vc_card_rec(k, n));
        }


        void merge(unsigned a, literal const* as, 
                   unsigned b, literal const* bs,                    
                   literal_vector& out) {
            TRACE("pb", tout << "merge a: " << a << " b: " << b << "\n";);
            if (a == 1 && b == 1) {
                literal y1 = max(as[0], bs[0]);
                literal y2 = min(as[0], bs[0]);
                out.push_back(y1);
                out.push_back(y2);
                cmp(as[0], bs[0], y1, y2);
            }
            else if (a == 0) {
                out.append(b, bs);
            }
            else if (b == 0) {
                out.append(a, as);
            }
            else if (use_dsmerge(a, b, a + b)) {
                dsmerge(a + b, a, as, b, bs, out);
            }
            else if (even(a) && odd(b)) {
                merge(b, bs, a, as, out);                
            }
            else {
                literal_vector even_a, odd_a;
                literal_vector even_b, odd_b;
                literal_vector out1, out2;
                SASSERT(a > 1 || b > 1);
                split(a, as, even_a, odd_a);
                split(b, bs, even_b, odd_b);
                SASSERT(!even_a.empty());
                SASSERT(!even_b.empty());
                merge(even_a.size(), even_a.c_ptr(),
                      even_b.size(), even_b.c_ptr(), out1);
                merge(odd_a.size(), odd_a.c_ptr(),
                      odd_b.size(), odd_b.c_ptr(), out2);
                interleave(out1, out2, out); 
            }
            TRACE("pb", tout << "merge a: " << a << " b: " << b << "\n";
                  pp(tout << "a:", a, as) << "\n";
                  pp(tout << "b:", b, bs) << "\n";
                  pp(tout << "out:", out) << "\n";);
        }
        vc vc_merge(unsigned a, unsigned b) {
            if (a == 1 && b == 1) {
                return vc_cmp();
            }
            else if (a == 0 || b == 0) {
                return vc(0, 0);
            }
            else if (use_dsmerge(a, b, a + b)) {
                return vc_dsmerge(a, b, a + b);
            }
            else {
                return vc_merge_rec(a, b);
            }
        }
        vc vc_merge_rec(unsigned a, unsigned b) {
            return 
                vc_merge(ceil2(a),  ceil2(b)) +
                vc_merge(floor2(a), floor2(b)) +
                vc_interleave(ceil2(a) + ceil2(b), floor2(a) + floor2(b));
        }
        void split(unsigned n, literal const* ls, literal_vector& even, literal_vector& odd) {
            for (unsigned i = 0; i < n; i += 2) {
                even.push_back(ls[i]);
            }
            for (unsigned i = 1; i < n; i += 2) {
                odd.push_back(ls[i]);
            }
        }

        void interleave(literal_vector const& as, 
                        literal_vector const& bs,
                        literal_vector& out) {
            TRACE("pb", tout << "interleave: " << as.size() << " " << bs.size() << "\n";);
            SASSERT(as.size() >= bs.size());
            SASSERT(as.size() <= bs.size() + 2);
            SASSERT(!as.empty());
            out.push_back(as[0]);
            unsigned sz = std::min(as.size()-1, bs.size());
            for (unsigned i = 0; i < sz; ++i) {
                literal y1 = max(as[i+1],bs[i]);
                literal y2 = min(as[i+1],bs[i]);
                cmp(as[i+1], bs[i], y1, y2);
                out.push_back(y1);
                out.push_back(y2);
            }            
            if (as.size() == bs.size()) {
                out.push_back(bs[sz]);
            }
            else if (as.size() == bs.size() + 2) {
                out.push_back(as[sz+1]);
            }
            SASSERT(out.size() == as.size() + bs.size());
            TRACE("pb", tout << "interleave: " << as.size() << " " << bs.size() << "\n";
                  pp(tout << "a: ", as) << "\n";
                  pp(tout << "b: ", bs) << "\n";
                  pp(tout << "out: ", out) << "\n";);

        }
        vc vc_interleave(unsigned a, unsigned b) {
            return vc_cmp()*std::min(a-1,b);
        }
        
        void sorting(unsigned n, literal const* xs, literal_vector& out) {
            TRACE("pb", tout << "sorting: " << n << "\n";);
            switch(n) {
            case 0: 
                break;
            case 1: 
                out.push_back(xs[0]); 
                break;
            case 2: 
                merge(1, xs, 1, xs+1, out); 
                break;
            default:
                if (use_dsorting(n)) {
                    dsorting(n, n, xs, out);
                }
                else {
                    literal_vector out1, out2;
                    unsigned l = n/2;  // TBD
                    sorting(l, xs, out1);
                    sorting(n-l, xs+l, out2);
                    merge(out1.size(), out1.c_ptr(), 
                          out2.size(), out2.c_ptr(),
                          out);                
                }
                break;
            }
            TRACE("pb", tout << "sorting: " << n << "\n";
                  pp(tout << "in:", n, xs) << "\n"; 
                  pp(tout << "out:", out) << "\n";);

        }
        vc vc_sorting(unsigned n) {
            switch(n) {
            case 0: return vc(0,0);
            case 1: return vc(0,0);
            case 2: return vc_merge(1,1);
            default:
                if (use_dsorting(n)) {
                    return vc_dsorting(n, n);
                }
                else {
                    return vc_sorting_rec(n);
                }
            }
        }
        vc vc_sorting_rec(unsigned n) {
            SASSERT(n > 2);
            unsigned l = n/2;
            return vc_sorting(l) + vc_sorting(n-l) + vc_merge(l, n-l);
        }

        bool use_dsorting(unsigned n) {
            SASSERT(n > 2);            
            return m_force_dsorting || 
                (!m_disable_dsorting && n < 10 && vc_dsorting(n, n) < vc_sorting_rec(n));
        }

        void smerge(unsigned c,
                    unsigned a, literal const* as,
                    unsigned b, literal const* bs, 
                    literal_vector& out) {
            TRACE("pb", tout << "smerge: c:" << c << " a:" << a << " b:" << b << "\n";);
            if (a == 1 && b == 1 && c == 1) {
                literal y = max(as[0], bs[0]);
                if (m_t != GE) {
                    // x1 <= max(x1,x2)
                    // x2 <= max(x1,x2)
                    add_clause(~as[0], y);
                    add_clause(~bs[0], y);
                }
                if (m_t != LE) {
                    // max(x1,x2) <= x1, x2
                    add_clause(~y, as[0], bs[0]);
                }
                out.push_back(y);
            }
            else if (a == 0) {
                out.append(std::min(c, b), bs);
            }
            else if (b == 0) {
                out.append(std::min(c, a), as);
            }
            else if (a > c) {
                smerge(c, c, as, b, bs, out);
            }
            else if (b > c) {
                smerge(c, a, as, c, bs, out);
            }
            else if (a + b <= c) {
                merge(a, as, b, bs, out);
            }
            else if (use_dsmerge(a, b, c)) {
                dsmerge(c, a, as, b, bs, out);
            }
            else {
                literal_vector even_a, odd_a;
                literal_vector even_b, odd_b;
                literal_vector out1, out2;               
                split(a, as, even_a, odd_a);
                split(b, bs, even_b, odd_b);
                SASSERT(!even_a.empty());
                SASSERT(!even_b.empty());
                unsigned c1, c2;
                if (even(c)) {
                    c1 = 1 + c/2; c2 = c/2;
                }
                else {
                    c1 = (c + 1)/2; c2 = (c - 1)/2;
                }
                smerge(c1, even_a.size(), even_a.c_ptr(),
                       even_b.size(), even_b.c_ptr(), out1);
                smerge(c2, odd_a.size(), odd_a.c_ptr(),
                       odd_b.size(), odd_b.c_ptr(), out2);
                SASSERT(out1.size() == std::min(even_a.size()+even_b.size(), c1));
                SASSERT(out2.size() == std::min(odd_a.size()+odd_b.size(), c2));
                literal y;
                if (even(c)) {
                    literal z1 = out1.back();
                    literal z2 = out2.back();
                    out1.pop_back();
                    out2.pop_back();
                    y = max(z1, z2);
                    if (m_t != GE) {
                        add_clause(~z1, y);
                        add_clause(~z2, y);
                    }
                    if (m_t != LE) {
                        add_clause(~y, z1, z2);
                    }
                }
                interleave(out1, out2, out);                 
                if (even(c)) {
                    out.push_back(y);
                }
            }
            TRACE("pb", tout << "smerge: c:" << c << " a:" << a << " b:" << b << "\n";
                  pp(tout << "a:", a, as) << "\n";
                  pp(tout << "b:", b, bs) << "\n";
                  pp(tout << "out:", out) << "\n";
                  );
            SASSERT(out.size() == std::min(a + b, c));
        }

        vc vc_smerge(unsigned a, unsigned b, unsigned c) {
            if (a == 1 && b == 1 && c == 1) {
                vc v(1,0);
                if (m_t != GE) v = v + vc(0, 2);
                if (m_t != LE) v = v + vc(0, 1);
                return v;
            }
            if (a == 0 || b == 0) return vc(0, 0);
            if (a > c) return vc_smerge(c, b, c);
            if (b > c) return vc_smerge(a, c, c);
            if (a + b <= c) return vc_merge(a, b);
            if (use_dsmerge(a, b, c)) return vc_dsmerge(a, b, c);
            return vc_smerge_rec(a, b, c);
        }
        vc vc_smerge_rec(unsigned a, unsigned b, unsigned c) {
            return 
                vc_smerge(ceil2(a), ceil2(b), even(c)?(1+c/2):((c+1)/2)) +
                vc_smerge(floor2(a), floor2(b), even(c)?(c/2):((c-1)/2)) +
                vc_interleave(ceil2(a)+ceil2(b),floor2(a)+floor2(b)) +
                vc(1, 0) +
                ((m_t != GE)?vc(0, 2):vc(0, 0)) +
                ((m_t != LE)?vc(0, 1):vc(0, 0));
        }
        bool use_dsmerge(unsigned a, unsigned b, unsigned c) {
            return 
                m_force_dsmerge ||
                (!m_disable_dsmerge && 
                 a < (1 << 15) && b < (1 << 15) && 
                 vc_dsmerge(a, b, a + b) < vc_smerge_rec(a, b, c));
        }

        void dsmerge(            
            unsigned c,
            unsigned a, literal const* as, 
            unsigned b, literal const* bs,                    
            literal_vector& out) {
            TRACE("pb", tout << "dsmerge: c:" << c << " a:" << a << " b:" << b << "\n";);
            SASSERT(a <= c);
            SASSERT(b <= c);
            SASSERT(a + b > c);
            for (unsigned i = 0; i < c; ++i) {
                out.push_back(fresh());
            }
            if (m_t != GE) {
                for (unsigned i = 0; i < a; ++i) {
                    add_clause(~as[i], out[i]);
                }
                for (unsigned i = 0; i < b; ++i) {
                    add_clause(~bs[i], out[i]);
                }
                for (unsigned i = 1; i <= a; ++i) {
                    for (unsigned j = 1; j <= b && i + j <= c; ++j) {
                        add_clause(~as[i-1],~bs[j-1],out[i+j-1]);
                    }
                }
            }
            if (m_t != LE) {
                for (unsigned k = 1; k <= c; ++k) {
                    literal_vector ls;
                    ls.push_back(~out[k-1]);
                    if (k <= a) {
                        ls.push_back(as[k-1]);
                    }
                    if (k <= b) {
                        ls.push_back(bs[k-1]);
                    }
                    for (unsigned i = 1; i <= std::min(a,k-1); ++i) {
                        if (k + 1 - i <= b) {
                            ls.push_back(as[i-1]);
                            ls.push_back(bs[k-i]);
                            add_clause(ls.size(), ls.c_ptr());
                            ls.pop_back();
                            ls.pop_back();
                        }
                    }
                }
            }
        }
        vc vc_dsmerge(unsigned a, unsigned b, unsigned c) {
            vc v(c, 0);
            if (m_t != GE) {
                v = v + vc(0, a + b + std::min(a, c)*std::min(b, c)/2);
            }
            if (m_t != LE) {
                v = v + vc(0, std::min(a, c)*std::min(b, c)/2);
            }
            return v;
        }


        void dsorting(unsigned m, unsigned n, literal const* xs, 
                      literal_vector& out) {
            TRACE("pb", tout << "dsorting m: " << m << " n: " << n << "\n";);
            SASSERT(m <= n);
            literal_vector lits;
            for (unsigned i = 0; i < m; ++i) {
                out.push_back(fresh());
            }
            if (m_t != GE) {
                for (unsigned k = 1; k <= m; ++k) {
                    lits.push_back(out[k-1]);
                    add_subset(true, k, 0, lits, n, xs);
                    lits.pop_back();
                }
            }
            if (m_t != LE) {                
                for (unsigned k = 1; k <= m; ++k) {
                    lits.push_back(~out[k-1]);
                    add_subset(false, n-k+1, 0, lits, n, xs);
                    lits.pop_back();
                }
            }
        }
        vc vc_dsorting(unsigned m, unsigned n) {
            SASSERT(m <= n && n < 10);            
            vc v(m, 0);
            if (m_t != GE) {
                v = v + vc(0, power2(n-1));
            }
            if (m_t != LE) {
                v = v + vc(0, power2(n-1));
            }
            return v;
        }

        void add_subset(bool polarity, unsigned k, unsigned offset, literal_vector& lits, 
                        unsigned n, literal const* xs) {
            TRACE("pb", tout << "k:" << k << " offset: " << offset << " n: " << n << " ";
                  pp(tout, lits) << "\n";);
            SASSERT(k + offset <= n);
            if (k == 0) {
                add_clause(lits.size(), lits.c_ptr());
                return;
            }
            for (unsigned i = offset; i < n - k + 1; ++i) {
                lits.push_back(polarity?~xs[i]:xs[i]);
                add_subset(polarity, k-1, i+1, lits, n, xs);
                lits.pop_back();
            }
        }
    };

    // for testing
    literal theory_pb::assert_ge(context& ctx, unsigned k, unsigned n, literal const* xs) {
        psort_nw sort(ctx);
        return sort.ge(false, k, n, xs);
    }

    class pb_lit_rewriter_util {
    public:
        typedef std::pair<literal, rational> arg_t;
        typedef vector<arg_t> args_t;
        typedef rational numeral;
        
        literal negate(literal l) {
            return ~l;
        }

        void display(std::ostream& out, literal l) {
            out << l;
        }
        
        bool is_negated(literal l) const {
            return l.sign();
        }
        
        bool is_true(literal l) const {
            return l == true_literal;
        }
        
        bool is_false(literal l) const {
            return l == false_literal;
        }
        
        struct compare {
            bool operator()(arg_t const& a, arg_t const& b) {
                return a.first < b.first;
            }
        };
    };

    void theory_pb::ineq::negate() {
        m_lit.neg();
        numeral sum(0);
        for (unsigned i = 0; i < size(); ++i) {
            m_args[i].first.neg();
            sum += coeff(i);
        }
        m_k = sum - m_k + numeral::one();
        VERIFY(l_undef == normalize());
        SASSERT(well_formed());
    }

    void theory_pb::ineq::reset() {
        m_max_watch.reset();
        m_watch_sz = 0;
        m_watch_sum.reset();
        m_num_propagations = 0;
        m_compilation_threshold = UINT_MAX;
        m_compiled = l_false;
        m_args.reset();
        m_k.reset();
    }


    void theory_pb::ineq::unique() {
        pb_lit_rewriter_util pbu;
        pb_rewriter_util<pb_lit_rewriter_util> util(pbu);
        util.unique(m_args, m_k);        
    }

    void theory_pb::ineq::prune() {
        pb_lit_rewriter_util pbu;
        pb_rewriter_util<pb_lit_rewriter_util> util(pbu);
        util.prune(m_args, m_k);        
    }

    lbool theory_pb::ineq::normalize() {
        pb_lit_rewriter_util pbu;
        pb_rewriter_util<pb_lit_rewriter_util> util(pbu);
        return util.normalize(m_args, m_k);        
    }

    app_ref theory_pb::ineq::to_expr(context& ctx, ast_manager& m) {
        expr_ref tmp(m);
        app_ref result(m);
        svector<rational> coeffs;
        expr_ref_vector args(m);
        for (unsigned i = 0; i < size(); ++i) {
            ctx.literal2expr(lit(i), tmp);
            args.push_back(tmp);
            coeffs.push_back(coeff(i));
        }
        pb_util pb(m);
        result = pb.mk_ge(coeffs.size(), coeffs.c_ptr(), args.c_ptr(), k());
        return result;
    }

    bool theory_pb::ineq::well_formed() const {
        SASSERT(k().is_pos());
        uint_set vars;
        numeral sum = numeral::zero();
        for (unsigned i = 0; i < size(); ++i) {
            SASSERT(coeff(i) <= k());
            SASSERT(numeral::one() <= coeff(i));
            SASSERT(lit(i) != true_literal);
            SASSERT(lit(i) != false_literal);
            SASSERT(lit(i) != null_literal);
            SASSERT(!vars.contains(lit(i).var()));
            vars.insert(lit(i).var());
            sum += coeff(i);
        }
        SASSERT(sum >= k());
        return true;
    }
    
    theory_pb::theory_pb(ast_manager& m, theory_pb_params& p):
        theory(m.mk_family_id("pb")),
        m_params(p),
        m_util(m),
        m_lemma(null_literal)
    {        
        m_learn_complements = p.m_pb_learn_complements;
        m_conflict_frequency = p.m_pb_conflict_frequency;
        m_enable_compilation = p.m_pb_enable_compilation;
    }

    theory_pb::~theory_pb() {
        reset_eh();
    }

    theory * theory_pb::mk_fresh(context * new_ctx) { 
        return alloc(theory_pb, new_ctx->get_manager(), m_params); 
    }

    bool theory_pb::internalize_atom(app * atom, bool gate_ctx) {
        context& ctx   = get_context();
        ast_manager& m = get_manager();
        unsigned num_args = atom->get_num_args();
        SASSERT(m_util.is_at_most_k(atom) || m_util.is_le(atom) || m_util.is_ge(atom));

        if (ctx.b_internalized(atom)) {
            return false;
        }

        m_stats.m_num_predicates++;

        SASSERT(!ctx.b_internalized(atom));
        bool_var abv = ctx.mk_bool_var(atom);
        ctx.set_var_theory(abv, get_id());

        ineq* c = alloc(ineq, literal(abv));
        c->m_k = m_util.get_k(atom);
        numeral& k = c->m_k;
        arg_t& args = c->m_args;

        // extract literals and coefficients.
        for (unsigned i = 0; i < num_args; ++i) {
            expr* arg = atom->get_arg(i);
            literal l = compile_arg(arg);
            numeral c = m_util.get_coeff(atom, i);
            args.push_back(std::make_pair(l, c));
        }
        if (m_util.is_at_most_k(atom) || m_util.is_le(atom)) {
            // turn W <= k into -W >= -k
            for (unsigned i = 0; i < args.size(); ++i) {
                args[i].second = -args[i].second;
            }
            k = -k;
        }
        else {
            SASSERT(m_util.is_at_least_k(atom) || m_util.is_ge(atom));
        }
        c->unique();
        lbool is_true = c->normalize();
        c->prune();

        literal lit(abv);

        TRACE("pb", display(tout << mk_pp(atom, m), *c); tout << " := " << lit << "\n";);        
        switch(is_true) {
        case l_false: 
            lit = ~lit;
            // fall-through
        case l_true: 
            ctx.mk_th_axiom(get_id(), 1, &lit);
            dealloc(c);
            return true;
        case l_undef: 
            break;
        }
            
        if (c->k().is_one()) {
            literal_vector& lits = get_lits();
            lits.push_back(~lit);
            for (unsigned i = 0; i < c->size(); ++i) {
                lits.push_back(c->lit(i));
                SASSERT(c->coeff(i).is_one());
                ctx.mk_th_axiom(get_id(), lit, ~c->lit(i));
            }
            ctx.mk_th_axiom(get_id(), lits.size(), lits.c_ptr());
            return true;
        }
        
        // maximal coefficient:
        numeral& max_watch = c->m_max_watch;
        max_watch = numeral::zero();
        for (unsigned i = 0; i < args.size(); ++i) {
            max_watch = std::max(max_watch, args[i].second);
        }


        // pre-compile threshold for cardinality
        bool enable_compile = m_enable_compilation;
        for (unsigned i = 0; enable_compile && i < args.size(); ++i) {
            enable_compile = (args[i].second < rational(8));
        }
        if (enable_compile) {
            unsigned log = 1, n = 1;
            while (n <= args.size()) {
                ++log;
                n *= 2;
            }
            unsigned th = args.size()*log; // 10*
            c->m_compilation_threshold = th;
            IF_VERBOSE(2, verbose_stream() << "(smt.pb setting compilation threhshold to " << th << ")\n";);
            TRACE("pb", tout << "compilation threshold: " << th << "\n";);
            }
        else {
            c->m_compilation_threshold = UINT_MAX;
        }
        m_ineqs.insert(abv, c);
        m_ineqs_trail.push_back(abv);

        TRACE("pb", display(tout, *c););

        return true;
    }

    literal theory_pb::compile_arg(expr* arg) {
        context& ctx = get_context();
        ast_manager& m = get_manager();

        bool_var bv;
        bool has_bv = false;
        bool negate = m.is_not(arg, arg);
        SASSERT(!m.is_not(arg));
        if (!ctx.b_internalized(arg)) {
            ctx.internalize(arg, false);
        }
        if (ctx.b_internalized(arg)) {
            bv = ctx.get_bool_var(arg);
            if (is_uninterp(arg) && null_theory_var == ctx.get_var_theory(bv)) {
                ctx.set_var_theory(bv, get_id());
            }
            has_bv = (ctx.get_var_theory(bv) == get_id());
        }
        else if (m.is_true(arg)) {
            bv = true_bool_var;
            has_bv = true;
        }
        else if (m.is_false(arg)) {
            bv = true_bool_var;
            has_bv = true;
            negate = !negate;
        }


        // assumes relevancy level = 2 or 0.
        // TBD: should should have been like an uninterpreted
        // function intenalize, where enodes for each argument
        // is available. 
        if (!has_bv) {
            expr_ref tmp(m), fml(m);
            tmp = m.mk_fresh_const("pb_proxy",m.mk_bool_sort());
            fml = m.mk_iff(tmp, arg);
            TRACE("pb", tout << "create proxy " << fml << "\n";);
            ctx.internalize(fml, false);
            SASSERT(ctx.b_internalized(tmp));
            bv = ctx.get_bool_var(tmp);
            SASSERT(null_theory_var == ctx.get_var_theory(bv));
            ctx.set_var_theory(bv, get_id());
            literal lit(ctx.get_bool_var(fml));
            ctx.mk_th_axiom(get_id(), 1, &lit);
            ctx.mark_as_relevant(tmp);
        }
        return negate?~literal(bv):literal(bv);
    }

    void theory_pb::del_watch(watch_list& watch, unsigned index, ineq& c, unsigned ineq_index) {
        if (index < watch.size()) {
            std::swap(watch[index], watch[watch.size()-1]);
        }
        watch.pop_back();
        
        SASSERT(ineq_index < c.watch_size());
        numeral coeff = c.coeff(ineq_index);
        if (ineq_index + 1 < c.watch_size()) {
            std::swap(c.m_args[ineq_index], c.m_args[c.watch_size()-1]);
        }
        --c.m_watch_sz;
        c.m_watch_sum  -= coeff;
        if (c.max_watch() == coeff) {
            coeff = c.coeff(0);
            for (unsigned i = 1; coeff != c.max_watch() && i < c.watch_size(); ++i) {
                if (coeff < c.coeff(i)) coeff = c.coeff(i);
            }
            c.set_max_watch(coeff);
        }

        // current index of unwatched literal is c.watch_size().
    }

    void theory_pb::add_watch(ineq& c, unsigned i) {
        literal lit = c.lit(i);
        bool_var v = lit.var();
        numeral coeff = c.coeff(i);
        c.m_watch_sum += coeff;
        SASSERT(i >= c.watch_size());
        
        if (i > c.watch_size()) {
            std::swap(c.m_args[i], c.m_args[c.watch_size()]);
        }
        ++c.m_watch_sz;
        if (coeff > c.max_watch()) {
            c.set_max_watch(coeff);
        }

        ptr_vector<ineq>* ineqs;
        if (!m_watch.find(lit.index(), ineqs)) {
            ineqs = alloc(ptr_vector<ineq>);
            m_watch.insert(lit.index(), ineqs);
        }
        ineqs->push_back(&c);
    }

    void theory_pb::collect_statistics(::statistics& st) const {
        st.update("pb conflicts", m_stats.m_num_conflicts);
        st.update("pb propagations", m_stats.m_num_propagations);
        st.update("pb predicates", m_stats.m_num_predicates);        
        st.update("pb compilations", m_stats.m_num_compiles);
        st.update("pb compiled clauses", m_stats.m_num_compiled_clauses);
        st.update("pb compiled vars", m_stats.m_num_compiled_vars);
    }
    
    void theory_pb::reset_eh() {
        
        // m_watch;
        u_map<ptr_vector<ineq>*>::iterator it = m_watch.begin(), end = m_watch.end();
        for (; it != end; ++it) {
            dealloc(it->m_value);
        }
        u_map<ineq*>::iterator itc = m_ineqs.begin(), endc = m_ineqs.end();
        for (; itc != endc; ++itc) {
            dealloc(itc->m_value);
        }
        m_watch.reset();
        m_ineqs.reset();
        m_ineqs_trail.reset();
        m_ineqs_lim.reset();
        m_assign_ineqs_trail.reset();
        m_assign_ineqs_lim.reset();
        m_stats.reset();
        m_to_compile.reset();
    }

    void theory_pb::new_eq_eh(theory_var v1, theory_var v2) {
        IF_VERBOSE(0, verbose_stream() << v1 << " = " << v2 << "\n";);
    }
    
    final_check_status theory_pb::final_check_eh() {
        TRACE("pb", display(tout););
        DEBUG_CODE(validate_final_check(););
        return FC_DONE;
    }

    void theory_pb::assign_eh(bool_var v, bool is_true) {
        context& ctx = get_context();
        ptr_vector<ineq>* ineqs = 0;
        literal nlit(v, is_true);
        TRACE("pb", tout << "assign: " << ~nlit << "\n";);
        if (m_watch.find(nlit.index(), ineqs)) {
            for (unsigned i = 0; i < ineqs->size(); ++i) {
                if (assign_watch(v, is_true, *ineqs, i)) {
                    // i was removed from watch list.
                    --i;
                }
            }
        }
        ineq* c = 0;
        if (m_ineqs.find(v, c)) {
            assign_ineq(*c, is_true);
        }
    }

    literal_vector& theory_pb::get_helpful_literals(ineq& c, bool negate) {
        numeral sum = numeral::zero();
        context& ctx = get_context();
        literal_vector& lits = get_lits();
        for (unsigned i = 0; sum < c.k() && i < c.size(); ++i) {
            literal l = c.lit(i);
            if (ctx.get_assignment(l) == l_true) {
                sum += c.coeff(i);
                if (negate) l = ~l;
                lits.push_back(l);
            }
        }
        SASSERT(sum >= c.k());        
        return lits;
    }

    literal_vector& theory_pb::get_unhelpful_literals(ineq& c, bool negate) {
        context& ctx = get_context();
        literal_vector& lits = get_lits();
        for (unsigned i = 0; i < c.size(); ++i) {
            literal l = c.lit(i);
            if (ctx.get_assignment(l) == l_false) {
                if (negate) l = ~l;
                lits.push_back(l);
            }
        }
        return lits;
    }

    /**
       \brief propagate assignment to inequality.
       This is a basic, non-optimized implementation based
       on the assumption that inequalities are mostly units
       and/or relatively few compared to number of argumets.
     */
    void theory_pb::assign_ineq(ineq& c, bool is_true) {

        if (c.lit().sign() == is_true) {
            c.negate();
        }
        SASSERT(c.well_formed());

        context& ctx = get_context();
        numeral maxsum = numeral::zero();
        numeral mininc = numeral::zero();
        for (unsigned i = 0; i < c.size(); ++i) {
            lbool asgn = ctx.get_assignment(c.lit(i));
            if (asgn != l_false) {
                maxsum += c.coeff(i);
            }
            if (asgn == l_undef && (mininc.is_zero() || mininc > c.coeff(i))) {
                mininc = c.coeff(i);
            }
        }

        TRACE("pb", 
              tout << "assign: " << c.lit() << "\n";
              display(tout, c); );

        if (maxsum < c.k()) {
            literal_vector& lits = get_unhelpful_literals(c, false);
            lits.push_back(~c.lit());
            add_clause(c, lits);
        }
        else {
            c.m_watch_sum = numeral::zero();
            c.m_watch_sz = 0;
            c.m_max_watch = numeral::zero();
            for (unsigned i = 0; c.watch_sum() < c.k() + c.max_watch() && i < c.size(); ++i) {
                if (ctx.get_assignment(c.lit(i)) != l_false) {
                    add_watch(c, i);
                }       
            }
            SASSERT(c.watch_sum() >= c.k());
            m_assign_ineqs_trail.push_back(&c);
            DEBUG_CODE(validate_watch(c););
        }

        // perform unit propagation
        if (maxsum >= c.k() && maxsum - mininc < c.k()) { 
            literal_vector& lits = get_unhelpful_literals(c, true);
            lits.push_back(c.lit());
            for (unsigned i = 0; i < c.size(); ++i) {
                if (ctx.get_assignment(c.lit(i)) == l_undef) {
                    DEBUG_CODE(validate_assign(c, lits, c.lit(i)););
                    add_assign(c, lits, c.lit(i));
                }                
            }
        }
    }

    /**
       \brief v is assigned in inequality c. Update current bounds and watch list.
       Optimize for case where the c.lit() is True. This covers the case where 
       inequalities are unit literals and formulas in negation normal form 
       (inequalities are closed under negation).
       
     */
    bool theory_pb::assign_watch(bool_var v, bool is_true, watch_list& watch, unsigned watch_index) {
        bool removed = false;
        context& ctx = get_context();
        ineq& c = *watch[watch_index];
        unsigned w = c.find_lit(v, 0, c.watch_size());

        SASSERT(ctx.get_assignment(c.lit()) == l_true);
        SASSERT(is_true == c.lit(w).sign());

        //
        // watch_sum is decreased.
        // Adjust set of watched literals.
        //
        
        numeral k = c.k();
        numeral coeff = c.coeff(w);
        bool add_more = c.watch_sum() - coeff < k + c.max_watch();
        for (unsigned i = c.watch_size(); add_more && i < c.size(); ++i) {
            if (ctx.get_assignment(c.lit(i)) != l_false) {
                add_watch(c, i);
                add_more = c.watch_sum() - coeff < k + c.max_watch();
            }
        }        
        
        if (c.watch_sum() - coeff < k) {
            //
            // L: 3*x1 + 2*x2 + x4 >= 3, but x1 <- 0, x2 <- 0
            // create clause x1 or x2 or ~L
            // 
            literal_vector& lits = get_unhelpful_literals(c, false);
            lits.push_back(~c.lit());
            add_clause(c, lits);
        }
        else {
            del_watch(watch, watch_index, c, w);
            removed = true;
            SASSERT(c.watch_sum() >= k);
            if (c.watch_sum() < k + c.max_watch()) {
                
                //
                // opportunities for unit propagation for unassigned 
                // literals whose coefficients satisfy
                // c.watch_sum() < k
                //
                // L: 3*x1 + 2*x2 + x4 >= 3, but x1 <- 0
                // Create clauses x1 or ~L or x2 
                //                x1 or ~L or x4
                //
                
                literal_vector& lits = get_unhelpful_literals(c, true);
                lits.push_back(c.lit());
                numeral deficit = c.watch_sum() - k;
                for (unsigned i = 0; i < c.size(); ++i) {
                    if (ctx.get_assignment(c.lit(i)) == l_undef && deficit < c.coeff(i)) {
                        DEBUG_CODE(validate_assign(c, lits, c.lit(i)););
                        add_assign(c, lits, c.lit(i));                            
                    }
                }
            }
            //
            // else: c.watch_sum() >= k + c.max_watch()
            //
        }

        TRACE("pb", 
              tout << "assign: " << literal(v,!is_true) << "\n";
              display(tout, c); );


        return removed;
    }

    struct theory_pb::sort_expr {
        theory_pb& th;
        context&     ctx;
        ast_manager& m;
        expr_ref_vector m_trail;
        sort_expr(theory_pb& th):
            th(th), 
            ctx(th.get_context()), 
            m(th.get_manager()), 
            m_trail(m) {}
        typedef expr* T;
        typedef expr_ref_vector vector;

        T mk_ite(T a, T b, T c) { 
            if (m.is_true(a)) {
                return b;
            }
            if (m.is_false(a)) {
                return c;
            }
            if (b == c) {
                return b;
            }
            m_trail.push_back(m.mk_ite(a, b, c));
            return m_trail.back();
        }

        T mk_le(T a, T b) {
            return mk_ite(b, a, m.mk_true());
        }

        T mk_default() {
            return m.mk_false();
        }       

        literal internalize(ineq& ca, expr* e) {
            obj_map<expr, literal> cache;
            expr_ref_vector trail(m);
            for (unsigned i = 0; i < ca.size(); ++i) {
                expr_ref tmp(m);
                ctx.literal2expr(ca.lit(i), tmp);
                cache.insert(tmp, ca.lit(i));
                trail.push_back(tmp);
            }
            cache.insert(m.mk_false(), false_literal);
            cache.insert(m.mk_true(),  true_literal);
            ptr_vector<expr> todo;
            todo.push_back(e);
            expr *a, *b, *c;
            literal la, lb, lc;
            while (!todo.empty()) {
                expr* t = todo.back();
                if (cache.contains(t)) {
                    todo.pop_back();
                    continue;
                }
                VERIFY(m.is_ite(t, a, b, c));
                unsigned sz = todo.size();
                if (!cache.find(a, la)) {
                    todo.push_back(a);
                }
                if (!cache.find(b, lb)) {
                    todo.push_back(b);
                }
                if (!cache.find(c, lc)) {
                    todo.push_back(c);
                }
                if (sz != todo.size()) {
                    continue;
                }
                todo.pop_back();
                cache.insert(t, mk_ite(ca, t, la, lb, lc));                
            }
            return cache.find(e);
        }

        literal mk_ite(ineq& ca, expr* t, literal a, literal b, literal c) {
            if (a == true_literal) {
                return b;
            }
            else if (a == false_literal) {
                return c;
            }
            else if (b == true_literal && c == false_literal) {
                return a;
            }
            else if (b == false_literal && c == true_literal) {
                return ~a;
            }
            else if (b == c) {
                return b;
            }
            else {                
                literal l;
                if (ctx.b_internalized(t)) {
                    l = literal(ctx.get_bool_var(t));
                }
                else {
                    l = literal(ctx.mk_bool_var(t));
                    ++th.m_stats.m_num_compiled_vars;
                }
                add_clause(~l, ~a,  b);
                add_clause(~l,  a,  c);
                add_clause(l,  ~a, ~b);
                add_clause(l,   a, ~c);
                TRACE("pb", tout << mk_pp(t, m) << " ::= (if ";
                      ctx.display_detailed_literal(tout, a);
                      ctx.display_detailed_literal(tout << " ", b);
                      ctx.display_detailed_literal(tout << " ", c);
                      tout << ")\n";);
                return l;
            }
        }


        // auxiliary clauses don't get garbage collected.
        void add_clause(literal a, literal b, literal c) {
            literal_vector& lits = th.get_lits();
            if (a != null_literal) lits.push_back(a); 
            if (b != null_literal) lits.push_back(b); 
            if (c != null_literal) lits.push_back(c);         
            justification* js = 0;
            TRACE("pb",
                  ctx.display_literals_verbose(tout, lits.size(), lits.c_ptr()); tout << "\n";);
            ctx.mk_clause(lits.size(), lits.c_ptr(), js, CLS_AUX, 0);
            ++th.m_stats.m_num_compiled_clauses;
        }            

        void add_clause(literal l1, literal l2) {
            add_clause(l1, l2, null_literal);
        }

    };


    void theory_pb::inc_propagations(ineq& c) {
        ++c.m_num_propagations;
        if (c.m_compiled == l_false && c.m_num_propagations > c.m_compilation_threshold) {
            c.m_compiled = l_undef;
            m_to_compile.push_back(&c);
        }
    }

    void theory_pb::restart_eh() {
        for (unsigned i = 0; i < m_to_compile.size(); ++i) {
            compile_ineq(*m_to_compile[i]);
        }
        m_to_compile.reset();
    }

    void theory_pb::compile_ineq(ineq& c) {
        ++m_stats.m_num_compiles;
        ast_manager& m = get_manager();
        context& ctx = get_context();
        // only cardinality constraints are compiled.
        SASSERT(c.m_compilation_threshold < UINT_MAX);
        DEBUG_CODE(for (unsigned i = 0; i < c.size(); ++i) SASSERT(c.coeff(i).is_int()); );
        unsigned k = c.k().get_unsigned();
        unsigned num_args = c.size();


        literal thl = c.lit();
        literal at_least_k;

#if 1
        literal_vector in;
        for (unsigned i = 0; i < num_args; ++i) {
            rational n = c.coeff(i);
            lbool val = ctx.get_assignment(c.lit()); 
            if (val != l_undef  && 
                ctx.get_assign_level(thl) == ctx.get_base_level()) {
                if (val == l_true) {
                    unsigned m = n.get_unsigned();
                    if (k < m) {
                        return;
                    }
                    k -= m;
                }
                continue;
            }
            while (n.is_pos()) {
                in.push_back(c.lit(i));
                n -= rational::one();
            }
        }
        if (ctx.get_assignment(thl) == l_true  && 
            ctx.get_assign_level(thl) == ctx.get_base_level()) {
            psort_nw sortnw(ctx);
            sortnw.m_stats.reset();
            at_least_k = sortnw.ge(false, k, in.size(), in.c_ptr());
            ctx.mk_clause(~thl, at_least_k, 0);
            m_stats.m_num_compiled_vars += sortnw.m_stats.m_num_compiled_vars;
            m_stats.m_num_compiled_clauses += sortnw.m_stats.m_num_compiled_clauses;
        }
        else {
            psort_nw sortnw(ctx);
            sortnw.m_stats.reset();
            literal at_least_k = sortnw.ge(true, k, in.size(), in.c_ptr());
            ctx.mk_clause(~thl, at_least_k, 0);
            ctx.mk_clause(~at_least_k, thl, 0);
            m_stats.m_num_compiled_vars += sortnw.m_stats.m_num_compiled_vars;
            m_stats.m_num_compiled_clauses += sortnw.m_stats.m_num_compiled_clauses;
        }
#else
        expr_ref_vector in(m), out(m);
        expr_ref tmp(m);
        for (unsigned i = 0; i < num_args; ++i) {
            rational n = c.coeff(i);
            ctx.literal2expr(c.lit(i), tmp);
            while (n.is_pos()) {
                in.push_back(tmp);
                n -= rational::one();
            }
        }
        sort_expr se(*this);
        sorting_network<sort_expr> sn(se);
        sn(in, out);
        at_least_k = se.internalize(c, out[k-1].get()); // first k outputs are 1.
        TRACE("pb", tout << "at_least: " << mk_pp(out[k-1].get(), m) << "\n";);
        
        se.add_clause(~thl, at_least_k);
        se.add_clause(thl, ~at_least_k);
#endif
        IF_VERBOSE(1, verbose_stream() 
                   << "(smt.pb compile sorting network bound: " 
                   << k << " literals: " << in.size() << ")\n";);

        TRACE("pb", tout << thl << "\n";);
        // auxiliary clauses get removed when popping scopes.
        // we have to recompile the circuit after back-tracking.
        c.m_compiled = l_false;
        ctx.push_trail(value_trail<context, lbool>(c.m_compiled));
        c.m_compiled = l_true;
    }


    void theory_pb::init_search_eh() {
        m_to_compile.reset();
    }

    void theory_pb::push_scope_eh() {
        m_ineqs_lim.push_back(m_ineqs_trail.size());
        m_assign_ineqs_lim.push_back(m_assign_ineqs_trail.size());
    }

    void theory_pb::pop_scope_eh(unsigned num_scopes) {

        // remove watched literals.
        unsigned new_lim = m_assign_ineqs_lim.size()-num_scopes;
        unsigned sz = m_assign_ineqs_lim[new_lim];
        while (m_assign_ineqs_trail.size() > sz) {
            ineq* c = m_assign_ineqs_trail.back();
            for (unsigned i = 0; i < c->watch_size(); ++i) {
                literal w = c->lit(i);
                ptr_vector<ineq>* ineqs = 0;
                VERIFY(m_watch.find(w.index(), ineqs));
                for (unsigned j = 0; j < ineqs->size(); ++j) {
                    if ((*ineqs)[j] == c) {                        
                        std::swap((*ineqs)[j],(*ineqs)[ineqs->size()-1]);
                        ineqs->pop_back();
                        break;
                    }
                }
            }
            m_assign_ineqs_trail.pop_back();
        }
        m_assign_ineqs_lim.resize(new_lim);

        // remove inequalities.
        new_lim = m_ineqs_lim.size()-num_scopes;
        sz = m_ineqs_lim[new_lim];
        while (m_ineqs_trail.size() > sz) {
            bool_var v = m_ineqs_trail.back();
            ineq* c = 0;
            VERIFY(m_ineqs.find(v, c));
            m_ineqs.remove(v);
            m_ineqs_trail.pop_back(); 
            dealloc(c);
        }
        m_ineqs_lim.resize(new_lim);
    }

    literal_vector& theory_pb::get_lits() {
        m_literals.reset();
        return m_literals;
    }

    class theory_pb::pb_justification : public theory_propagation_justification {
        ineq& m_ineq;
    public:
        pb_justification(ineq& c, family_id fid, region & r, 
                      unsigned num_lits, literal const * lits, literal consequent):
                      theory_propagation_justification(fid, r, num_lits, lits, consequent),
                      m_ineq(c)
                      {}
        ineq& get_ineq() { return m_ineq; }
    };

    void theory_pb::add_assign(ineq& c, literal_vector const& lits, literal l) {
        inc_propagations(c);
        m_stats.m_num_propagations++;
        context& ctx = get_context();
        TRACE("pb", tout << "#prop:" << c.m_num_propagations << " - "; 
              for (unsigned i = 0; i < lits.size(); ++i) {
                  tout << lits[i] << " ";
              }
              tout << "=> " << l << "\n";
              display(tout, c, true););


        ctx.assign(l, ctx.mk_justification(
                       pb_justification(
                           c, get_id(), ctx.get_region(), lits.size(), lits.c_ptr(), l)));
    }
    
                   

    void theory_pb::add_clause(ineq& c, literal_vector const& lits) {
        inc_propagations(c);
        m_stats.m_num_conflicts++;
        context& ctx = get_context();
        TRACE("pb", tout << "#prop:" << c.m_num_propagations << " - "; 
              for (unsigned i = 0; i < lits.size(); ++i) {
                  tout << lits[i] << " ";
              }
              tout << "\n";
              display(tout, c, true););

        justification* js = 0;

        if (m_conflict_frequency == 0 || (0 == (c.m_num_propagations % m_conflict_frequency))) {
            resolve_conflict(c);
        }

        ctx.mk_clause(lits.size(), lits.c_ptr(), js, CLS_AUX_LEMMA, 0);
    }


    void theory_pb::set_mark(bool_var v, unsigned idx) {
        SASSERT(v != null_bool_var);
        if (v >= static_cast<bool_var>(m_conseq_index.size())) {
            m_conseq_index.resize(v+1, null_index);
        }
        SASSERT(!is_marked(v) || m_conseq_index[v] == idx);
        m_marked.push_back(v);
        m_conseq_index[v] = idx;        
    }

    bool theory_pb::is_marked(bool_var v) const {
        return 
            (v < static_cast<bool_var>(m_conseq_index.size())) &&
            (m_conseq_index[v] != null_index);
    }

    void theory_pb::unset_mark(bool_var v) {
        SASSERT(v != null_bool_var);
        if (v < static_cast<bool_var>(m_conseq_index.size())) {
            m_conseq_index[v] = null_index;
        }
    }

    void theory_pb::unset_marks() {
        for (unsigned i = 0; i < m_marked.size(); ++i) {
            unset_mark(m_marked[i]);
        }
        m_marked.reset();
    }

    void theory_pb::process_antecedent(literal l, numeral coeff) {        
        context& ctx = get_context();
        bool_var v = l.var();
        unsigned lvl = ctx.get_assign_level(v);

        if (ctx.get_assignment(l) != l_false) {
            m_lemma.m_k -= coeff;
            if (m_learn_complements && is_marked(v)) {
                SASSERT(ctx.get_assignment(l) == l_true);
                numeral& lcoeff = m_lemma.m_args[m_conseq_index[v]].second; 
                lcoeff -= coeff;
                if (!lcoeff.is_pos()) {
                    // perhaps let lemma simplification change coefficient
                    // when negative?
                    remove_from_lemma(m_lemma, m_conseq_index[v]);                    
                }
            }
        }
        else if (lvl > ctx.get_base_level()) {
            if (is_marked(v)) {
                m_lemma.m_args[m_conseq_index[v]].second += coeff;
                SASSERT(m_lemma.m_args[m_conseq_index[v]].second.is_pos());
            }
            else {                
                if (lvl == m_conflict_lvl) {
                    TRACE("pb", tout << "add mark: " << l << " " << coeff << "\n";);
                    ++m_num_marks;
                }
                set_mark(v, m_lemma.size());
                m_lemma.m_args.push_back(std::make_pair(l, coeff));
            }
            TRACE("pb_verbose", tout  
                  << "ante: " << m_lemma.lit(m_conseq_index[v]) << "*" 
                  << m_lemma.coeff(m_conseq_index[v]) << " " << lvl << "\n";);
        }
    }

    void theory_pb::process_ineq(ineq& c, literal conseq, numeral coeff1) {

        //
        // Create CUT.
        //

        // 
        // . find coeff2
        // . find lcm of coefficients to conseq.
        // . multiply m_lemma by lcm/coeff coefficient to align.
        // . create lcm/coeff_2 to multiply on this side.
        // . cut resolve constraints.
        // 

        context& ctx = get_context();
        numeral coeff2 = (conseq==null_literal)?numeral::one():numeral::zero();
        for (unsigned i = 0; i < c.size(); ++i) {
            if (c.lit(i) == conseq) {
                coeff2 = c.coeff(i);
                break;
            }
        }
        SASSERT(coeff2.is_pos());
        numeral lc = lcm(coeff1, coeff2);
        numeral g = lc/coeff1;
        SASSERT(g.is_int());
        if (g > numeral::one()) {
            for (unsigned i = 0; i < m_lemma.size(); ++i) {
                m_lemma.m_args[i].second *= g;
            }
            m_lemma.m_k *= g;
        }
        g = lc/coeff2;
        SASSERT(g.is_int());
        m_lemma.m_k += g*c.k();

        for (unsigned i = 0; i < c.size(); ++i) {
            process_antecedent(c.lit(i), g*c.coeff(i));
        }

        SASSERT(ctx.get_assignment(c.lit()) == l_true);
        if (ctx.get_assign_level(c.lit()) > ctx.get_base_level()) {
            m_ineq_literals.push_back(c.lit());
        }
    }

    //
    // modeled after sat_solver/smt_context
    //
    bool theory_pb::resolve_conflict(ineq& c) {
        
        TRACE("pb", display(tout, c, true););

        bool_var v;
        literal conseq;
        context& ctx = get_context();
        unsigned& lvl = m_conflict_lvl = 0;
        for (unsigned i = 0; i < c.size(); ++i) {
            if (ctx.get_assignment(c.lit(i)) == l_false) {
                lvl = std::max(lvl, ctx.get_assign_level(c.lit(i)));
            }
        }
        if (lvl < ctx.get_assign_level(c.lit()) || lvl == ctx.get_base_level()) {
            return false;
        }

        unset_marks();
        m_num_marks = 0;
        m_lemma.reset();
        m_ineq_literals.reset();
        process_ineq(c, null_literal, numeral::one()); // add consequent to lemma.

        // point into stack of assigned literals
        literal_vector const& lits = ctx.assigned_literals();        
        SASSERT(!lits.empty());
        unsigned idx = lits.size()-1;
   
        while (m_num_marks > 0) {

            TRACE("pb_verbose", display(tout << "lemma ", m_lemma, true););

            lbool is_sat = m_lemma.normalize();
            if (is_sat == l_false) {
                break;
            }
            if (is_sat == l_true) {
                IF_VERBOSE(0, verbose_stream() << "lemma already evaluated ";);
                TRACE("pb", tout << "lemma already evaluated ";);
                return false;
            }
            TRACE("pb", display(tout, m_lemma, true););
            SASSERT(m_lemma.well_formed());         

            //
            // find the next marked variable in the assignment stack
            //
            do {
                conseq = lits[idx];
                v = conseq.var();
                --idx;
            }
            while (!is_marked(v) && idx > 0);
            if (idx == 0 && !is_marked(v)) {
                //
                // Yes, this can (currently) happen because
                // the decisions for performing unit propagation
                // are made asynchronously.
                // In other words, PB unit propagation does not follow the
                // same order as the assignment stack.
                // It is not a correctness bug but causes to miss lemmas.
                //
                IF_VERBOSE(2, display_resolved_lemma(verbose_stream()););
                TRACE("pb", display_resolved_lemma(tout););
                return false;
            }
            
            unsigned conseq_index = m_conseq_index[v];
            numeral conseq_coeff = m_lemma.coeff(conseq_index);

            TRACE("pb", display(tout, m_lemma, true);
                  tout << "conseq: " << conseq << " at index: " << conseq_index << "\n";);

            SASSERT(~conseq == m_lemma.lit(conseq_index));

            remove_from_lemma(m_lemma, conseq_index);

            b_justification js = ctx.get_justification(v);

            //
            // Resolve selected conseq with antecedents.
            // 

            switch(js.get_kind()) {
                
            case b_justification::CLAUSE: {
                clause& cls = *js.get_clause();
                justification* cjs = cls.get_justification();
                if (cjs && !is_proof_justification(*cjs)) {
                    TRACE("pb", tout << "skipping justification for clause over: " << conseq << " " 
                          << typeid(*cjs).name() << "\n";);
                    m_ineq_literals.push_back(conseq);
                    break;
                }
                unsigned num_lits = cls.get_num_literals();
                if (cls.get_literal(0) == conseq) {
                    process_antecedent(cls.get_literal(1), conseq_coeff);
                }
                else {
                    SASSERT(cls.get_literal(1) == conseq);
                    process_antecedent(cls.get_literal(0), conseq_coeff);
                }
                for (unsigned i = 2; i < num_lits; ++i) {
                    process_antecedent(cls.get_literal(i), conseq_coeff);
                }
                TRACE("pb", for (unsigned i = 0; i < num_lits; ++i) tout << cls.get_literal(i) << " "; tout << "\n";);
                break;                
            }
            case b_justification::BIN_CLAUSE:
                process_antecedent(~js.get_literal(), conseq_coeff);
                TRACE("pb", tout << "binary: " << js.get_literal() << "\n";);
                break;
            case b_justification::AXIOM:
                if (ctx.get_assign_level(v) > ctx.get_base_level()) {
                    m_ineq_literals.push_back(conseq);
                }
                TRACE("pb", tout << "axiom " << conseq << "\n";);
                break;
            case b_justification::JUSTIFICATION: {
                justification& j = *js.get_justification(); 
                if (j.get_from_theory() != get_id()) {                    
                    TRACE("pb", tout << "skipping justification for " << conseq 
                          << " from theory "  << j.get_from_theory() << " " 
                          << typeid(j).name() << "\n";);
                    m_ineq_literals.push_back(conseq);
                }
                else {
                    pb_justification& pbj = dynamic_cast<pb_justification&>(j);
                    // weaken the lemma and resolve.
                    TRACE("pb", display(tout << "resolve with inequality", pbj.get_ineq(), true););
                    process_ineq(pbj.get_ineq(), conseq, conseq_coeff);
                }
                break;
            }
            default:
                UNREACHABLE();
            }            
        }

        TRACE("pb", 
              for (unsigned i = 0; i < m_ineq_literals.size(); ++i) {
                  tout << m_ineq_literals[i] << " ";
              }
              display(tout << "=> ", m_lemma););

        // 3x + 3y + z + u >= 4
        // ~x /\ ~y => z + u >= 

        IF_VERBOSE(4, display(verbose_stream() << "lemma1: ", m_lemma););
        hoist_maximal_values();
        lbool is_true = m_lemma.normalize();
        m_lemma.prune();

        IF_VERBOSE(4, display(verbose_stream() << "lemma2: ", m_lemma););
        switch(is_true) {
        case l_true:
            UNREACHABLE();
            return false;
        case l_false: 
            inc_propagations(c);
            m_stats.m_num_conflicts++;
            for (unsigned i = 0; i < m_ineq_literals.size(); ++i) {
                m_ineq_literals[i].neg();
            }
            ctx.mk_clause(m_ineq_literals.size(), m_ineq_literals.c_ptr(), 0, CLS_AUX_LEMMA, 0);
            break;
        default: {
            app_ref tmp = m_lemma.to_expr(ctx, get_manager());
            internalize_atom(tmp, false);
            ctx.mark_as_relevant(tmp);
            literal l(ctx.get_bool_var(tmp));
            add_assign(c, m_ineq_literals, l);
            break;
        }
        }
        return true;    
    }

    bool theory_pb::is_proof_justification(justification const& j) const {
        return typeid(smt::justification_proof_wrapper) == typeid(j);
    }


    void theory_pb::hoist_maximal_values() {
        for (unsigned i = 0; i < m_lemma.size(); ++i) {
            if (m_lemma.coeff(i) >= m_lemma.k()) {
                m_ineq_literals.push_back(~m_lemma.lit(i));
                std::swap(m_lemma.m_args[i], m_lemma.m_args[m_lemma.size()-1]);
                m_lemma.m_args.pop_back();
                --i;
            }
        }
    }

    void theory_pb::remove_from_lemma(ineq& c, unsigned idx) {
        // Remove conseq from lemma:
        literal lit = c.lit(idx);
        unsigned last = c.size()-1;
        if (idx != last) {
            c.m_args[idx] = c.m_args[last];
            m_conseq_index[c.lit(idx).var()] = idx;
        }
        c.m_args.pop_back();
        unset_mark(lit.var());
        --m_num_marks;
    }


    // debug methods

    void theory_pb::validate_watch(ineq const& c) const {
        context& ctx = get_context();
        numeral sum = numeral::zero();
        numeral max = numeral::zero();
        for (unsigned i = 0; i < c.watch_size(); ++i) {
            sum += c.coeff(i);
            if (c.coeff(i) > max) {
                max = c.coeff(i);
            }
        }
        SASSERT(c.watch_sum() == sum);
        SASSERT(sum >= c.k());
        SASSERT(max == c.max_watch());
    }

    void theory_pb::validate_assign(ineq const& c, literal_vector const& lits, literal l) const {
        uint_set nlits;
        context& ctx = get_context();
        for (unsigned i = 0; i < lits.size(); ++i) {
            SASSERT(ctx.get_assignment(lits[i]) == l_true);
            nlits.insert((~lits[i]).index());
        }
        SASSERT(ctx.get_assignment(l) == l_undef);
        SASSERT(ctx.get_assignment(c.lit()) == l_true);
        nlits.insert(l.index());
        numeral sum = numeral::zero();
        for (unsigned i = 0; i < c.size(); ++i) {
            literal lit = c.lit(i);
            if (!nlits.contains(lit.index())) {
                sum += c.coeff(i);
            }
        }
        CTRACE("pb", (sum >= c.k()),
               display(tout << "invalid assign" , c, true);
               for (unsigned i = 0; i < lits.size(); ++i) {
                   tout << lits[i] << " ";
               }
               tout << " => " << l << "\n";);
        SASSERT(sum < c.k());
    }

    void theory_pb::validate_final_check() {
        u_map<ineq*>::iterator itc = m_ineqs.begin(), endc = m_ineqs.end();
        for (; itc != endc; ++itc) {
            validate_final_check(*itc->m_value);                
        }
    }

    void theory_pb::validate_final_check(ineq& c) {
        context& ctx = get_context();

        if (ctx.get_assignment(c.lit()) == l_undef) {
            return;
        }
        if (!ctx.is_relevant(c.lit())) {
            return;
        }
        numeral sum = numeral::zero(), maxsum = numeral::zero();
        for (unsigned i = 0; i < c.size(); ++i) {
            switch(ctx.get_assignment(c.lit(i))) {
            case l_true:
                sum += c.coeff(i);
            case l_undef:
                maxsum += c.coeff(i);
                break;
            }
        }
        TRACE("pb", display(tout << "validate: ", c, true);
              tout << "sum: " << sum << " " << maxsum << " ";
              tout << ctx.get_assignment(c.lit()) << "\n";
              );

        SASSERT(sum <= maxsum);
        SASSERT((sum >= c.k()) == (ctx.get_assignment(c.lit()) == l_true));
        SASSERT((maxsum < c.k()) == (ctx.get_assignment(c.lit()) == l_false));
    }

    // display methods

    void theory_pb::display_resolved_lemma(std::ostream& out) const {
        context& ctx = get_context();
        literal_vector const& lits = ctx.assigned_literals();                
        bool_var v;
        unsigned lvl;
        out << "num marks: " << m_num_marks << "\n";
        out << "conflict level: " << m_conflict_lvl << "\n";
        for (unsigned i = 0; i < lits.size(); ++i) {
            v = lits[i].var();
            lvl = ctx.get_assign_level(v);
            out << lits[i]
                << "@ " << lvl 
                << " " << (is_marked(v)?"m":"u") 
                << "\n";
            
            if (lvl == m_conflict_lvl && is_marked(v)) {
                out << "skipped: " << lits[i] << ":"<< i << "\n";
            }
        }
        display(out, m_lemma, true); 

        unsigned nc = 0;
        for (unsigned i = 0; i < m_lemma.size(); ++i) {
            v = m_lemma.lit(i).var();
            lvl = ctx.get_assign_level(v);
            if (lvl == m_conflict_lvl) ++nc;
            out << m_lemma.lit(i) 
                << "@" << lvl 
                << " " << (is_marked(v)?"m":"u") 
                << " " << ctx.get_assignment(m_lemma.lit(i))
                << "\n";
        }
        out << "num conflicts: " << nc << "\n";
    }


    std::ostream& theory_pb::display(std::ostream& out, ineq const& c, bool values) const {
        ast_manager& m = get_manager();
        context& ctx = get_context();
        out << c.lit();
        if (c.lit() != null_literal) {
            if (values) {
                out << "@(" << ctx.get_assignment(c.lit());
                if (ctx.get_assignment(c.lit()) != l_undef) {
                    out << ":" << ctx.get_assign_level(c.lit());
                }
                out << ")";
            }
            expr_ref tmp(m);
            ctx.literal2expr(c.lit(), tmp);
            out << " " << tmp << "\n";
        }
        else {
            out << " ";
        }
        for (unsigned i = 0; i < c.size(); ++i) {
            literal l(c.lit(i));
            if (!c.coeff(i).is_one()) {
                out << c.coeff(i) << "*";
            }
            out << l;
            if (values) {
                out << "@(" << ctx.get_assignment(l);
                if (ctx.get_assignment(l) != l_undef) {
                    out << ":" << ctx.get_assign_level(l);
                }
                out << ")";
            }
            if (i + 1 == c.watch_size()) {
                out << " .w ";
            }
            if (i + 1 < c.size()) {
                out << " + ";
            }
        }
        out << " >= " << c.m_k  << "\n";
        if (c.m_num_propagations)    out << "propagations: " << c.m_num_propagations << " ";
        if (c.max_watch().is_pos())  out << "max_watch: "    << c.max_watch() << " ";
        if (c.watch_size())          out << "watch size: "   << c.watch_size() << " ";
        if (c.watch_sum().is_pos())  out << "watch-sum: "    << c.watch_sum() << " ";
        if (c.m_num_propagations || c.max_watch().is_pos() || c.watch_size() || c.watch_sum().is_pos()) out << "\n";
        return out;
    }

    class theory_pb::pb_model_value_proc : public model_value_proc {
        app*              m_app;
        svector<model_value_dependency> m_dependencies;
    public:

        pb_model_value_proc(app* a): 
            m_app(a) {}

        void add(enode* n) { 
            m_dependencies.push_back(model_value_dependency(n)); 
        }

        virtual void get_dependencies(buffer<model_value_dependency> & result) {
            result.append(m_dependencies.size(), m_dependencies.c_ptr());
        }

        virtual app * mk_value(model_generator & mg, ptr_vector<expr> & values) {
            ast_manager& m = mg.get_manager();
            SASSERT(values.size() == m_dependencies.size());
            SASSERT(values.size() == m_app->get_num_args());
            pb_util u(m);
            rational sum(0);
            for (unsigned i = 0; i < m_app->get_num_args(); ++i) {
                if (!m.is_true(values[i]) && !m.is_false(values[i])) {
                    return m_app;
                }
                if (m.is_true(values[i])) {
                    sum += u.get_coeff(m_app, i);
                }
            }
            rational k = u.get_k(m_app);
            switch(m_app->get_decl_kind()) {
            case OP_AT_MOST_K:
                return (sum <= k)?m.mk_true():m.mk_false();
            case OP_AT_LEAST_K:
                return (sum >= k)?m.mk_true():m.mk_false();
            case OP_PB_LE:
                return (sum <= k)?m.mk_true():m.mk_false();
            case OP_PB_GE:
                return (sum >= k)?m.mk_true():m.mk_false();
            default:
                UNREACHABLE();
                return 0;
            }
            return 0;
        }
    };

    class pb_factory : public value_factory {
    public:
        pb_factory(ast_manager& m, family_id fid):
            value_factory(m, fid) {}
        
        virtual expr * get_some_value(sort * s) {
            return m_manager.mk_true();
        }        
        virtual bool get_some_values(sort * s, expr_ref & v1, expr_ref & v2) {
            v1 = m_manager.mk_true();
            v2 = m_manager.mk_false();
            return true;
        }        
        virtual expr * get_fresh_value(sort * s) {
            return 0;
        }
        virtual void register_value(expr * n) { }
    };

    void theory_pb::init_model(model_generator & m) {
        m.register_factory(alloc(pb_factory, get_manager(), get_id()));
    }

    model_value_proc * theory_pb::mk_value(enode * n, model_generator & mg) {
        context& ctx = get_context();
        app* a = n->get_owner();
        pb_model_value_proc* p = alloc(pb_model_value_proc, a);
        for (unsigned i = 0; i < a->get_num_args(); ++i) {
            p->add(ctx.get_enode(a->get_arg(i)));
        }
        return p;
    }

    void theory_pb::display(std::ostream& out) const {
        u_map<ptr_vector<ineq>*>::iterator it = m_watch.begin(), end = m_watch.end();
        for (; it != end; ++it) {
            out << "watch: " << to_literal(it->m_key) << " |-> ";
            watch_list const& wl = *it->m_value;
            for (unsigned i = 0; i < wl.size(); ++i) {
                out << wl[i]->lit() << " ";
            }
            out << "\n";
        }
        u_map<ineq*>::iterator itc = m_ineqs.begin(), endc = m_ineqs.end();
        for (; itc != endc; ++itc) {
            ineq& c = *itc->m_value;
            display(out, c);
        }
    }


}
