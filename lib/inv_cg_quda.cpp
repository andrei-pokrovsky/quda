#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <quda_internal.h>
#include <color_spinor_field.h>
#include <blas_quda.h>
#include <dslash_quda.h>
#include <invert_quda.h>
#include <util_quda.h>
#include <sys/time.h>
#include <limits>
#include <cmath>

#include <face_quda.h>

#include <iostream>

#ifdef BLOCKSOLVER
#include <Eigen/Dense>

// define this to use multireduce, otherwise it'll
// do loops over dot products.
// this is more here for development convenience.
#define BLOCKSOLVER_MULTIREDUCE
//#define BLOCKSOLVER_VERBOSE

// Explicitly reorthogonalize Q^\dagger P on reliable update.
//#define BLOCKSOLVER_EXPLICIT_QP_ORTHO
// Explicitly make pAp Hermitian every time it is computed.
//#define BLOCKSOLVER_EXPLICIT_PAP_HERMITIAN

// If defined, trigger a reliable updated whenever _any_ residual
// becomes small enough. Otherwise, trigger a reliable update
// when _all_ residuals become small enough (which is consistent with
// the algorithm stopping condition). Ultimately, this is using a
// min function versus a max function, so it's not a hard swap.
// #define BLOCKSOLVER_RELIABLE_POLICY_MIN

#ifdef BLOCKSOLVER_NVTX
#include "nvToolsExt.h"
static const uint32_t cg_nvtx_colors[] = { 0x0000ff00, 0x000000ff, 0x00ffff00, 0x00ff00ff, 0x0000ffff, 0x00ff0000, 0x00ffffff };
static constexpr int cg_nvtx_num_colors = sizeof(cg_nvtx_colors)/sizeof(uint32_t);
#define PUSH_RANGE(name,cid) { \
    static int color_id = cid; \
    color_id = color_id%cg_nvtx_num_colors;\
    nvtxEventAttributes_t eventAttrib = {0}; \
    eventAttrib.version = NVTX_VERSION; \
    eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE; \
    eventAttrib.colorType = NVTX_COLOR_ARGB; \
    eventAttrib.color = cg_nvtx_colors[color_id]; \
    eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII; \
    eventAttrib.message.ascii = name; \
    eventAttrib.category = cid;\
    nvtxRangePushEx(&eventAttrib); \
}
#define POP_RANGE nvtxRangePop();
#else
#define PUSH_RANGE(name,cid)
#define POP_RANGE
#endif

#endif // BLOCKSOLVER

namespace quda {
  CG::CG(DiracMatrix &mat, DiracMatrix &matSloppy, SolverParam &param, TimeProfile &profile) :
    Solver(param, profile), mat(mat), matSloppy(matSloppy), yp(nullptr), rp(nullptr), App(nullptr), tmpp(nullptr),
#ifdef BLOCKSOLVER
    x_sloppy_savedp(nullptr), pp(nullptr), qp(nullptr), tmp_matsloppyp(nullptr),
#endif
    init(false) {
  }

  CG::~CG() {
    if ( init ) {
      if (rp) delete rp;
      if (yp) delete yp;
      if (App) delete App;
      if (tmpp) delete tmpp;
#ifdef BLOCKSOLVER
      if (x_sloppy_savedp) delete x_sloppy_savedp;
      if (pp) delete pp;
      if (qp) delete qp;
      if (tmp_matsloppyp) delete tmp_matsloppyp;
#endif
      init = false;
    }
  }

  void CG::operator()(ColorSpinorField &x, ColorSpinorField &b) {
    if (Location(x, b) != QUDA_CUDA_FIELD_LOCATION)
      errorQuda("Not supported");

#ifdef ALTRELIABLE
    // hack to select alternative reliable updates
    constexpr bool alternative_reliable = true;
    warningQuda("Using alternative reliable updates. This feature is mostly ok but needs a little more testing in the real world.\n");
#else
    constexpr bool alternative_reliable = false;
#endif
    profile.TPSTART(QUDA_PROFILE_INIT);

    // Check to see that we're not trying to invert on a zero-field source
    double b2 = blas::norm2(b);

    // Check to see that we're not trying to invert on a zero-field source
    if (b2 == 0 && param.compute_null_vector == QUDA_COMPUTE_NULL_VECTOR_NO) {
      profile.TPSTOP(QUDA_PROFILE_INIT);
      printfQuda("Warning: inverting on zero-field source\n");
      x = b;
      param.true_res = 0.0;
      param.true_res_hq = 0.0;
      return;
    }

    ColorSpinorParam csParam(x);
    if (!init) {
      csParam.create = QUDA_COPY_FIELD_CREATE;
      rp = ColorSpinorField::Create(b, csParam);
      csParam.create = QUDA_ZERO_FIELD_CREATE;
      yp = ColorSpinorField::Create(b, csParam);
      // sloppy fields
      csParam.setPrecision(param.precision_sloppy);
      App = ColorSpinorField::Create(csParam);
      tmpp = ColorSpinorField::Create(csParam);

      init = true;

    }
    ColorSpinorField &r = *rp;
    ColorSpinorField &y = *yp;
    ColorSpinorField &Ap = *App;
    ColorSpinorField &tmp = *tmpp;

    csParam.setPrecision(param.precision_sloppy);
    csParam.create = QUDA_ZERO_FIELD_CREATE;

    // tmp2 only needed for multi-gpu Wilson-like kernels
    ColorSpinorField *tmp2_p = !mat.isStaggered() ? ColorSpinorField::Create(x, csParam) : &tmp;
    ColorSpinorField &tmp2 = *tmp2_p;

    // additional high-precision temporary if Wilson and mixed-precision
    csParam.setPrecision(param.precision);
    ColorSpinorField *tmp3_p = (x.Precision() != param.precision_sloppy && !mat.isStaggered()) ?
      ColorSpinorField::Create(x, csParam) : &tmp;
    ColorSpinorField &tmp3 = *tmp3_p;

    // alternative reliable updates
    // alternative reliable updates - set precision - does not hurt performance here

    const double u= param.precision_sloppy == 8 ? std::numeric_limits<double>::epsilon()/2. : ((param.precision_sloppy == 4) ? std::numeric_limits<float>::epsilon()/2. : pow(2.,-13));
    const double uhigh= param.precision == 8 ? std::numeric_limits<double>::epsilon()/2. : ((param.precision == 4) ? std::numeric_limits<float>::epsilon()/2. : pow(2.,-13));
    const double deps=sqrt(u);
    constexpr double dfac = 1.1;
    double d_new =0 ;
    double d =0 ;
    double dinit =0;
    double xNorm = 0;
    double xnorm = 0;
    double pnorm = 0;
    double ppnorm = 0;
    double Anorm = 0;
    
    // for alternative reliable updates
    if(alternative_reliable){
      // estimate norm for reliable updates
      mat(r, b, y, tmp3);
      Anorm = sqrt(blas::norm2(r)/b2);
    }


    // compute initial residual
    mat(r, x, y, tmp3);
    double r2 = blas::xmyNorm(b, r);
    if (b2 == 0) {
      b2 = r2;
    }

    csParam.setPrecision(param.precision_sloppy);
    ColorSpinorField *r_sloppy;
    if (param.precision_sloppy == x.Precision()) {
      r_sloppy = &r;
    } else {
      csParam.create = QUDA_COPY_FIELD_CREATE;
      r_sloppy = ColorSpinorField::Create(r, csParam);
    }

    ColorSpinorField *x_sloppy;
    if (param.precision_sloppy == x.Precision() ||
        !param.use_sloppy_partial_accumulator) {
      x_sloppy = &x;
    } else {
      csParam.create = QUDA_COPY_FIELD_CREATE;
      x_sloppy = ColorSpinorField::Create(x, csParam);
    }

    ColorSpinorField &xSloppy = *x_sloppy;
    ColorSpinorField &rSloppy = *r_sloppy;

    csParam.create = QUDA_COPY_FIELD_CREATE;
    csParam.setPrecision(param.precision_sloppy);
    ColorSpinorField* pp = ColorSpinorField::Create(rSloppy, csParam);
    ColorSpinorField &p = *pp;

    if (&x != &xSloppy) {
      blas::copy(y, x);
      blas::zero(xSloppy);
    } else {
      blas::zero(y);
    }

    const bool use_heavy_quark_res =
      (param.residual_type & QUDA_HEAVY_QUARK_RESIDUAL) ? true : false;
    bool heavy_quark_restart = false;

    profile.TPSTOP(QUDA_PROFILE_INIT);
    profile.TPSTART(QUDA_PROFILE_PREAMBLE);

    double r2_old;

    double stop = stopping(param.tol, b2, param.residual_type);  // stopping condition of solver

    double heavy_quark_res = 0.0;  // heavy quark res idual
    double heavy_quark_res_old = 0.0;  // heavy quark residual

    if (use_heavy_quark_res) {
      heavy_quark_res = sqrt(blas::HeavyQuarkResidualNorm(x, r).z);
      heavy_quark_res_old = heavy_quark_res;   // heavy quark residual
    }
    const int heavy_quark_check = param.heavy_quark_check; // how often to check the heavy quark residual

    double alpha = 0.0;
    double beta = 0.0;
    double pAp;
    int rUpdate = 0;

    double rNorm = sqrt(r2);
    double r0Norm = rNorm;
    double maxrx = rNorm;
    double maxrr = rNorm;
    double delta = param.delta;


    // this parameter determines how many consective reliable update
    // residual increases we tolerate before terminating the solver,
    // i.e., how long do we want to keep trying to converge
    const int maxResIncrease = (use_heavy_quark_res ? 0 : param.max_res_increase); //  check if we reached the limit of our tolerance
    const int maxResIncreaseTotal = param.max_res_increase_total;
    // 0 means we have no tolerance
    // maybe we should expose this as a parameter
    const int hqmaxresIncrease = maxResIncrease + 1;

    int resIncrease = 0;
    int resIncreaseTotal = 0;
    int hqresIncrease = 0;

    // set this to true if maxResIncrease has been exceeded but when we use heavy quark residual we still want to continue the CG
    // only used if we use the heavy_quark_res
    bool L2breakdown = false;

    profile.TPSTOP(QUDA_PROFILE_PREAMBLE);
    profile.TPSTART(QUDA_PROFILE_COMPUTE);
    blas::flops = 0;

    int k = 0;

    PrintStats("CG", k, r2, b2, heavy_quark_res);

    int steps_since_reliable = 1;
    bool converged = convergence(r2, heavy_quark_res, stop, param.tol_hq);

    // alternative reliable updates
    if(alternative_reliable){
      dinit = uhigh * (rNorm + Anorm * xNorm);
      d = dinit;
    }

    while ( !converged && k < param.maxiter ) {
      matSloppy(Ap, p, tmp, tmp2);  // tmp as tmp
      double sigma;

      bool breakdown = false;
      if (param.pipeline) {
        double Ap2;
        //TODO: alternative reliable updates - need r2, Ap2, pAp, p norm
        if(alternative_reliable){
          double4 quadruple = blas::quadrupleCGReduction(rSloppy, Ap, p);
          r2 = quadruple.x; Ap2 = quadruple.y; pAp = quadruple.z; ppnorm= quadruple.w;
        }
        else{
        double3 triplet = blas::tripleCGReduction(rSloppy, Ap, p);
          r2 = triplet.x; Ap2 = triplet.y; pAp = triplet.z;
        }
        r2_old = r2;
        alpha = r2 / pAp;
        sigma = alpha*(alpha * Ap2 - pAp);
        if (sigma < 0.0 || steps_since_reliable == 0) { // sigma condition has broken down
          r2 = blas::axpyNorm(-alpha, Ap, rSloppy);
          sigma = r2;
          breakdown = true;
        }

        r2 = sigma;
      } else {
        r2_old = r2;

        // alternative reliable updates,
        if(alternative_reliable){
          double3 pAppp = blas::cDotProductNormA(p,Ap);
          pAp = pAppp.x;
          ppnorm = pAppp.z;
        }
        else{
        pAp = blas::reDotProduct(p, Ap);
        }

        alpha = r2 / pAp;

        // here we are deploying the alternative beta computation
        Complex cg_norm = blas::axpyCGNorm(-alpha, Ap, rSloppy);
        r2 = real(cg_norm);  // (r_new, r_new)
        sigma = imag(cg_norm) >= 0.0 ? imag(cg_norm) : r2;  // use r2 if (r_k+1, r_k+1-r_k) breaks
      }

      // reliable update conditions
      rNorm = sqrt(r2);
      int updateX;
      int updateR;

      if(alternative_reliable){
        // alternative reliable updates
        updateX = ( (d <= deps*sqrt(r2_old)) or (dfac * dinit > deps * r0Norm) ) and (d_new > deps*rNorm) and (d_new > dfac * dinit);
        updateR = 0;
        // if(updateX)
          // printfQuda("new reliable update conditions (%i) d_n-1 < eps r2_old %e %e;\t dn > eps r_n %e %e;\t (dnew > 1.1 dinit %e %e)\n",
        // updateX,d,deps*sqrt(r2_old),d_new,deps*rNorm,d_new,dinit);
      }
      else{
      if (rNorm > maxrx) maxrx = rNorm;
      if (rNorm > maxrr) maxrr = rNorm;
        updateX = (rNorm < delta*r0Norm && r0Norm <= maxrx) ? 1 : 0;
        updateR = ((rNorm < delta*maxrr && r0Norm <= maxrr) || updateX) ? 1 : 0;
      }

      // force a reliable update if we are within target tolerance (only if doing reliable updates)
      if ( convergence(r2, heavy_quark_res, stop, param.tol_hq) && param.delta >= param.tol ) updateX = 1;

      // For heavy-quark inversion force a reliable update if we continue after
      if ( use_heavy_quark_res and L2breakdown and convergenceHQ(r2, heavy_quark_res, stop, param.tol_hq) and param.delta >= param.tol ) {
        updateX = 1;
      }

      if ( !(updateR || updateX )) {
        beta = sigma / r2_old;  // use the alternative beta computation


        if (param.pipeline && !breakdown) blas::tripleCGUpdate(alpha, beta, Ap, xSloppy, rSloppy, p);
  else blas::axpyZpbx(alpha, p, xSloppy, rSloppy, beta);

  if (use_heavy_quark_res && k%heavy_quark_check==0) {
    if (&x != &xSloppy) {
      blas::copy(tmp,y);
      heavy_quark_res = sqrt(blas::xpyHeavyQuarkResidualNorm(xSloppy, tmp, rSloppy).z);
    } else {
      blas::copy(r, rSloppy);
      heavy_quark_res = sqrt(blas::xpyHeavyQuarkResidualNorm(x, y, r).z);
    }
  }
  // alternative reliable updates
  if(alternative_reliable){
    d = d_new;
    pnorm = pnorm + alpha * alpha* (ppnorm);
    xnorm = sqrt(pnorm);
    d_new = d + u*rNorm + uhigh*Anorm * xnorm;
    if(steps_since_reliable==0)
      printfQuda("New dnew: %e (r %e , y %e)\n",d_new,u*rNorm,uhigh*Anorm * sqrt(blas::norm2(y)) );
  }
        steps_since_reliable++;

      } else {

        blas::axpy(alpha, p, xSloppy);
        blas::copy(x, xSloppy); // nop when these pointers alias

        blas::xpy(x, y); // swap these around?
        mat(r, y, x, tmp3); //  here we can use x as tmp
        r2 = blas::xmyNorm(b, r);

        blas::copy(rSloppy, r); //nop when these pointers alias
        blas::zero(xSloppy);

        // alternative reliable updates
        if(alternative_reliable){
          dinit = uhigh*(sqrt(r2) + Anorm * sqrt(blas::norm2(y)));
          d = d_new;
          xnorm = 0;//sqrt(norm2(x));
          pnorm = 0;//pnorm + alpha * sqrt(norm2(p));
          printfQuda("New dinit: %e (r %e , y %e)\n",dinit,uhigh*sqrt(r2),uhigh*Anorm*sqrt(blas::norm2(y)));
          d_new = dinit;
          r0Norm = sqrt(r2);
        }
        else{
          rNorm = sqrt(r2);
          maxrr = rNorm;
          maxrx = rNorm;
          r0Norm = rNorm;
        }


        // calculate new reliable HQ resididual
        if (use_heavy_quark_res) heavy_quark_res = sqrt(blas::HeavyQuarkResidualNorm(y, r).z);

        // break-out check if we have reached the limit of the precision
        if (sqrt(r2) > r0Norm && updateX) { // reuse r0Norm for this
          resIncrease++;
          resIncreaseTotal++;
          warningQuda("CG: new reliable residual norm %e is greater than previous reliable residual norm %e (total #inc %i)",
          sqrt(r2), r0Norm, resIncreaseTotal);
          if ( resIncrease > maxResIncrease or resIncreaseTotal > maxResIncreaseTotal) {
            if (use_heavy_quark_res) {
              L2breakdown = true;
            } else {
              warningQuda("CG: solver exiting due to too many true residual norm increases");
              break;
            }
          }
        } else {
          resIncrease = 0;
        }
        // if L2 broke down already we turn off reliable updates and restart the CG
        if (use_heavy_quark_res and L2breakdown) {
          delta = 0;
          warningQuda("CG: Restarting without reliable updates for heavy-quark residual");
          heavy_quark_restart = true;
          if (heavy_quark_res > heavy_quark_res_old) {
            hqresIncrease++;
            warningQuda("CG: new reliable HQ residual norm %e is greater than previous reliable residual norm %e", heavy_quark_res, heavy_quark_res_old);
            // break out if we do not improve here anymore
            if (hqresIncrease > hqmaxresIncrease) {
              warningQuda("CG: solver exiting due to too many heavy quark residual norm increases");
              break;
            }
          }
        }



        if (use_heavy_quark_res and heavy_quark_restart) {
          // perform a restart
          blas::copy(p, rSloppy);
          heavy_quark_restart = false;
        } else {
          // explicitly restore the orthogonality of the gradient vector
          Complex rp = blas::cDotProduct(rSloppy, p) / (r2);
          blas::caxpy(-rp, rSloppy, p);

          beta = r2 / r2_old;
          blas::xpay(rSloppy, beta, p);
        }

        steps_since_reliable = 0;
        rUpdate++;

        heavy_quark_res_old = heavy_quark_res;
      }

      breakdown = false;
      k++;

      PrintStats("CG", k, r2, b2, heavy_quark_res);
      // check convergence, if convergence is satisfied we only need to check that we had a reliable update for the heavy quarks recently
      converged = convergence(r2, heavy_quark_res, stop, param.tol_hq);

      // check for recent enough reliable updates of the HQ residual if we use it
      if (use_heavy_quark_res) {
        // L2 is concverged or precision maxed out for L2
        bool L2done = L2breakdown or convergenceL2(r2, heavy_quark_res, stop, param.tol_hq);
        // HQ is converged and if we do reliable update the HQ residual has been calculated using a reliable update
        bool HQdone = (steps_since_reliable == 0 and param.delta > 0) and convergenceHQ(r2, heavy_quark_res, stop, param.tol_hq);
        converged = L2done and HQdone;
      }
    }

    blas::copy(x, xSloppy);
    blas::xpy(y, x);

    profile.TPSTOP(QUDA_PROFILE_COMPUTE);

    profile.TPSTART(QUDA_PROFILE_EPILOGUE);

    param.secs = profile.Last(QUDA_PROFILE_COMPUTE);
    double gflops = (blas::flops + mat.flops() + matSloppy.flops())*1e-9;
    param.gflops = gflops;
    param.iter += k;

    { // temporary addition for SC'17
      comm_allreduce(&gflops);
      printfQuda("CG: Convergence in %d iterations, %f seconds, GFLOPS = %g\n", k, param.secs, gflops / param.secs);
    }

    if (k == param.maxiter)
      warningQuda("Exceeded maximum iterations %d", param.maxiter);

    if (getVerbosity() >= QUDA_VERBOSE)
      printfQuda("CG: Reliable updates = %d\n", rUpdate);

    if (param.compute_true_res) {
      // compute the true residuals
      mat(r, x, y, tmp3);
      param.true_res = sqrt(blas::xmyNorm(b, r) / b2);
      param.true_res_hq = sqrt(blas::HeavyQuarkResidualNorm(x, r).z);
    }

    PrintSummary("CG", k, r2, b2);

    // reset the flops counters
    blas::flops = 0;
    mat.flops();
    matSloppy.flops();

    profile.TPSTOP(QUDA_PROFILE_EPILOGUE);
    profile.TPSTART(QUDA_PROFILE_FREE);

    if (&tmp3 != &tmp) delete tmp3_p;
    if (&tmp2 != &tmp) delete tmp2_p;

    if (rSloppy.Precision() != r.Precision()) delete r_sloppy;
    if (xSloppy.Precision() != x.Precision()) delete x_sloppy;

    delete pp;

    profile.TPSTOP(QUDA_PROFILE_FREE);

    return;
  }


#ifdef BLOCKSOLVER

// use BlockCGrQ algortithm or BlockCG (with / without GS, see BLOCKCG_GS option)
#define BCGRQ 1
#if BCGRQ

#ifdef BLOCKSOLVER_MULTIREDUCE
  using Eigen::Matrix;
  using Eigen::Map;
  using Eigen::RowMajor;
  using Eigen::Dynamic; 

#elif defined(BLOCKSOLVER)
  using Eigen::MatrixXcd;
#endif

// Matrix printing functions

template<typename Matrix>
inline void printmat(const char* label, const Matrix& mat)
{
#ifdef BLOCKSOLVER_VERBOSE
  printfQuda("\n%s\n", label);
  std::cout << mat;
  printfQuda("\n");
#endif
}

/**
     The following code is based on Kate's worker class in Multi-CG.
     
     This worker class is used to perform the update of X_sloppy,
     UNLESS a reliable update is triggered. X_sloppy is updated
     via a block caxpy: X_sloppy += P \alpha.
     We can accomodate multiple comms-compute overlaps
     by partitioning the block caxpy w/respect to P, because
     this doesn't require any memory shuffling of the dense, square
     matrix alpha. This results in improved strong scaling for
     blockCG. 

     See paragraphs 2 and 3 in the comments on the Worker class in
     Multi-CG for more remarks. 
*/
class BlockCGUpdate : public Worker {

    ColorSpinorField* x_sloppyp;
    ColorSpinorField** pp; // double pointer because pp participates in pointer swapping
#ifdef BLOCKSOLVER_MULTIREDUCE
    Complex* alpha;
#else
    Complex* AC;
    MatrixXcd* alpha;
#endif

    /**
       How many RHS we're solving.
    */
    unsigned int n_rhs;

    /**
       How much to partition the shifted update. For now, we assume
       we always need to partition into two pieces (since BiCGstab-L
       should only be getting even/odd preconditioned operators).
    */
    int n_update; 

  public:
#ifdef BLOCKSOLVER_MULTIREDUCE
    BlockCGUpdate(ColorSpinorField* x_sloppyp, ColorSpinorField** pp, Complex* alpha) :
#else
    BlockCGUpdate(ColorSpinorField* x_sloppyp, ColorSpinorField** pp, MatrixXcd* alpha) :
#endif
      x_sloppyp(x_sloppyp), pp(pp), alpha(alpha), n_rhs((*pp)->Components().size()),
      n_update( x_sloppyp->Nspin()==4 ? 4 : 2 )
    {
#ifndef BLOCKSOLVER_MULTIREDUCE
      AC = new Complex[n_rhs*n_rhs];
#endif
    }
    ~BlockCGUpdate() {
#ifndef BLOCKSOLVER_MULTIREDUCE
      delete[] AC;
#endif
    }
    

    // note that we can't set the stream parameter here so it is
    // ignored.  This is more of a future design direction to consider
    void apply(const cudaStream_t &stream) {      
      static int count = 0;

      // How many to update per apply.
      const int update_per_apply = n_rhs/n_update;

      // If the number of updates doesn't evenly divide into n_rhs, there's leftover.
      const int update_per_apply_on_last = n_rhs - n_update*update_per_apply;

      // Only update if there are things to apply.
      // Update 1 through n_count-1, as well as n_count if update_per_apply_blah_blah = 0.
      PUSH_RANGE("BLAS",2)
      if ((count != n_update-1 && update_per_apply != 0) || update_per_apply_on_last == 0)
      {
        std::vector<ColorSpinorField*> curr_p((*pp)->Components().begin() + count*update_per_apply, (*pp)->Components().begin() + (count+1)*update_per_apply);

#ifdef BLOCKSOLVER_MULTIREDUCE
        blas::caxpy(&alpha[count*update_per_apply*n_rhs], curr_p, x_sloppyp->Components());
#else
        for (int i = 0; i < update_per_apply; i++)
          for (int j = 0; j < n_rhs; j++)
            AC[i*n_rhs + j] = alpha(i + count*update_per_apply, j);
        blas::caxpy(AC, curr_p, x_sloppyp->Components());
#endif
      }
      else if (count == n_update-1) // we're updating the leftover.
      {
        std::vector<ColorSpinorField*> curr_p((*pp)->Components().begin() + count*update_per_apply, (*pp)->Components().end());
#ifdef BLOCKSOLVER_MULTIREDUCE
        blas::caxpy(&alpha[count*update_per_apply*n_rhs], curr_p, x_sloppyp->Components());
#else
        for (int i = 0; i < update_per_apply_on_last; i++)
          for (int j = 0; j < n_rhs; j++)
            AC[i*n_rhs + j] = alpha(i + count*update_per_apply, j);
        blas::caxpy(AC, curr_p, x_sloppyp->Components());
#endif
      }
      POP_RANGE
      
      if (++count == n_update) count = 0;
      
    }
    
  };

  // this is the Worker pointer that the dslash uses to launch the shifted updates
  namespace dslash {
    extern Worker* aux_worker;
  } 

// Code to check for reliable updates, copied from inv_bicgstab_quda.cpp
// Technically, there are ways to check both 'x' and 'r' for reliable updates...
// the current status in blockCG is to just look for reliable updates in 'r'.
int CG::block_reliable(double &rNorm, double &maxrx, double &maxrr, const double &r2, const double &delta) {
  // reliable updates
  rNorm = sqrt(r2);
  if (rNorm > maxrx) maxrx = rNorm;
  if (rNorm > maxrr) maxrr = rNorm;
  //int updateR = (rNorm < delta*maxrr && r0Norm <= maxrr) ? 1 : 0;
  //int updateX = (rNorm < delta*r0Norm && r0Norm <= maxrx) ? 1 : 0
  int updateR = (rNorm < delta*maxrr) ? 1 : 0;
  
  //printf("reliable %d %e %e %e %e\n", updateR, rNorm, maxrx, maxrr, r2);

  return updateR;
}


template <int nsrc>
void CG::solve_n(ColorSpinorField& x, ColorSpinorField& b) {

  if (Location(x, b) != QUDA_CUDA_FIELD_LOCATION) errorQuda("Not supported");

  profile.TPSTART(QUDA_PROFILE_INIT);

  // Check to see that we're not trying to invert on a zero-field source
  //MW: it might be useful to check what to do here.
  double b2[QUDA_MAX_BLOCK_SRC];
  double b2avg=0;
  for(int i=0; i<nsrc; i++){
    b2[i]=blas::norm2(b.Component(i));
    b2avg += b2[i];
    if(b2[i] == 0){
      profile.TPSTOP(QUDA_PROFILE_INIT);
      errorQuda("Warning: inverting on zero-field source - undefined for block solver\n");
      x=b;
      param.true_res = 0.0;
      param.true_res_hq = 0.0;
      return;
    }
  }

  b2avg = b2avg / nsrc;

  ColorSpinorParam csParam(x);

  csParam.is_composite  = true;
  csParam.composite_dim = nsrc;
  csParam.nDim = 5;
  csParam.x[4] = 1;

  if (!init) {
    csParam.create = QUDA_COPY_FIELD_CREATE;
    rp = ColorSpinorField::Create(b, csParam);
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    yp = ColorSpinorField::Create(b, csParam);
    // sloppy fields
    csParam.setPrecision(param.precision_sloppy);
    x_sloppy_savedp = ColorSpinorField::Create(csParam);
    pp = ColorSpinorField::Create(csParam);
    qp = ColorSpinorField::Create(csParam);
    App = ColorSpinorField::Create(csParam);
    tmpp = ColorSpinorField::Create(csParam);
    tmp_matsloppyp = ColorSpinorField::Create(csParam);
    init = true;

  }
  ColorSpinorField &r = *rp;
  ColorSpinorField &y = *yp;
  ColorSpinorField &x_sloppy_saved = *x_sloppy_savedp;
  ColorSpinorField &Ap = *App;
  ColorSpinorField &tmp_matsloppy = *tmp_matsloppyp;

  ColorSpinorField *x_sloppyp; // Gets assigned below.

  r.ExtendLastDimension();
  y.ExtendLastDimension();
  x_sloppy_saved.ExtendLastDimension();
  Ap.ExtendLastDimension();
  pp->ExtendLastDimension();
  qp->ExtendLastDimension();
  tmpp->ExtendLastDimension();
  tmp_matsloppy.ExtendLastDimension();

  // calculate residuals for all vectors
  //for(int i=0; i<nsrc; i++){
  //  mat(r.Component(i), x.Component(i), y.Component(i));
  //  blas::xmyNorm(b.Component(i), r.Component(i));
  //}
  // Step 2: R = AX - B, using Y as a temporary with the right precision.
  mat(r, x, y);
  blas::xpay(b, -1.0, r);

  // Step 3: Y = X
  blas::copy(y, x);

  // Step 4: Xs = 0
  // Set field aliasing according to whether
  // we're doing mixed precision or not. Based
  // on BiCGstab-L conventions.
  if (param.precision_sloppy == x.Precision() || !param.use_sloppy_partial_accumulator)
  {
    x_sloppyp = &x; // s_sloppy and x point to the same vector in memory.
    blas::zero(*x_sloppyp); // x_sloppy is zeroed out (and, by extension, so is x)
  }
  else
  {
    x_sloppyp = x_sloppy_savedp; // x_sloppy point to saved x_sloppy memory.
                                 // x_sloppy_savedp was already zero.
  }
  // No need to alias r---we need a separate q, which is analagous
  // to an 'r_sloppy' in other algorithms.

  // Syntatic sugar.
  ColorSpinorField &x_sloppy = *x_sloppyp;

#ifdef BLOCKSOLVER_MULTIREDUCE
  // Set up eigen matrices here.
  // We need to do some goofing around with Eigen maps.
  // https://eigen.tuxfamily.org/dox/group__TutorialMapClass.html

  // Allocate some raw memory for each matrix we need raw pointers for.
  Complex H_raw[nsrc*nsrc];
  Complex pAp_raw[nsrc*nsrc];
  Complex alpha_raw[nsrc*nsrc];
  Complex beta_raw[nsrc*nsrc];
  Complex Linv_raw[nsrc*nsrc];
  Complex Sdagger_raw[nsrc*nsrc];

  // Convenience. By default, Eigen matrices are column major.
  // We switch to row major because cDotProduct and
  // the multi-blas routines use row
  typedef Matrix<Complex, nsrc, nsrc, RowMajor> MatrixBCG;

  // Create maps. This forces the above pointers to be used under the hood.
  Map<MatrixBCG> H(H_raw, nsrc, nsrc);
  Map<MatrixBCG> pAp(pAp_raw, nsrc, nsrc);
  Map<MatrixBCG> alpha(alpha_raw, nsrc, nsrc);
  Map<MatrixBCG> beta(beta_raw, nsrc, nsrc);
  Map<MatrixBCG> Linv(Linv_raw, nsrc, nsrc);
  Map<MatrixBCG> Sdagger(Sdagger_raw, nsrc, nsrc);

  // Create other non-mapped matrices.
  MatrixBCG L = MatrixBCG::Zero(nsrc,nsrc);
  MatrixBCG C = MatrixBCG::Zero(nsrc,nsrc);
  MatrixBCG C_old = MatrixBCG::Zero(nsrc,nsrc);
  MatrixBCG S = MatrixBCG::Identity(nsrc,nsrc); // Step 10: S = I

#ifdef BLOCKSOLVER_VERBOSE
  Complex* pTp_raw = new Complex[nsrc*nsrc];
  Map<MatrixBCG> pTp(pTp_raw,nsrc,nsrc);
#endif
#else
  // Eigen Matrices instead of scalars
  MatrixXcd H = MatrixXcd::Zero(nsrc, nsrc);
  MatrixXcd alpha = MatrixXcd::Zero(nsrc,nsrc);
  MatrixXcd beta = MatrixXcd::Zero(nsrc,nsrc);
  MatrixXcd C = MatrixXcd::Zero(nsrc,nsrc);
  MatrixXcd C_old = MatrixXcd::Zero(nsrc,nsrc);
  MatrixXcd S = MatrixXcd::Identity(nsrc,nsrc); // Step 10: S = I
  MatrixXcd Sdagger = MatrixXcd::Identity(nsrc,nsrc);
  MatrixXcd L = MatrixXcd::Zero(nsrc, nsrc);
  MatrixXcd Linv = MatrixXcd::Zero(nsrc, nsrc);
  MatrixXcd pAp = MatrixXcd::Identity(nsrc,nsrc);
  quda::Complex AC[nsrc*nsrc];

  #ifdef BLOCKSOLVER_VERBOSE
  MatrixXcd pTp =  MatrixXcd::Identity(nsrc,nsrc);
  #endif
#endif 

  // Step 5: H = (R)^\dagger R
  double r2avg=0;
#ifdef BLOCKSOLVER_MULTIREDUCE
  blas::hDotProduct(H_raw, r.Components(), r.Components());

  for (int i = 0; i < nsrc; i++)
  {
    r2avg += H(i,i).real();
    printfQuda("r2[%i] %e\n", i, H(i,i).real());
  }
#else
  for(int i=0; i<nsrc; i++){
    for(int j=i; j < nsrc; j++){
      H(i,j) = blas::cDotProduct(r.Component(i),r.Component(j));
      if (i!=j) H(j,i) = std::conj(H(i,j));
      if (i==j) {
        r2avg += H(i,i).real();
        printfQuda("r2[%i] %e\n", i, H(i,i).real());
      }
    }
  }
#endif
  printmat("r2", H);

  csParam.setPrecision(param.precision_sloppy);
  // tmp2 only needed for multi-gpu Wilson-like kernels
  //ColorSpinorField *tmp2_p = !mat.isStaggered() ?
  //ColorSpinorField::Create(x, csParam) : &tmp;
  //ColorSpinorField &tmp2 = *tmp2_p;
  ColorSpinorField *tmp2_p = nullptr;

  if(!mat.isStaggered()){
    csParam.create = QUDA_ZERO_FIELD_CREATE;
    tmp2_p =  ColorSpinorField::Create(csParam);
    tmp2_p->ExtendLastDimension();
  } else {
    tmp2_p = tmp_matsloppyp;
  }

  ColorSpinorField &tmp2 = *tmp2_p;

  // additional high-precision temporary if Wilson and mixed-precision
  csParam.setPrecision(param.precision);

  ColorSpinorField *tmp3_p = nullptr;
  if(param.precision != param.precision_sloppy && !mat.isStaggered()){
    //csParam.create = QUDA_ZERO_FIELD_CREATE;
    tmp3_p =  ColorSpinorField::Create(x, csParam); //ColorSpinorField::Create(csParam);
    tmp3_p->ExtendLastDimension();
  } else {
    tmp3_p = tmp_matsloppyp;
  }

  ColorSpinorField &tmp3 = *tmp3_p;

  //ColorSpinorParam cs5dParam(p);
  //cs5dParam.create = QUDA_REFERENCE_FIELD_CREATE;
  //cs5dParam.x[4] = nsrc;
  //cs5dParam.is_composite = false;
  
  //cudaColorSpinorField Ap5d(Ap,cs5dParam); 
  //cudaColorSpinorField p5d(p,cs5dParam); 
  //cudaColorSpinorField tmp5d(tmp,cs5dParam); 
  //cudaColorSpinorField tmp25d(tmp2,cs5dParam);  

  const bool use_heavy_quark_res =
  (param.residual_type & QUDA_HEAVY_QUARK_RESIDUAL) ? true : false;
  if(use_heavy_quark_res) errorQuda("ERROR: heavy quark residual not supported in block solver");

  // Create the worker class for updating x_sloppy. 
  // When we hit matSloppy, tmpp contains P.
#ifdef BLOCKSOLVER_MULTIREDUCE
  BlockCGUpdate blockcg_update(&x_sloppy, &tmpp, alpha_raw);
#else
  BlockCGUpdate blockcg_update(&x_sloppy, &tmpp, &alpha);
#endif

  profile.TPSTOP(QUDA_PROFILE_INIT);
  profile.TPSTART(QUDA_PROFILE_PREAMBLE);

  double stop[QUDA_MAX_BLOCK_SRC];

  for(int i = 0; i < nsrc; i++){
    stop[i] = stopping(param.tol, b2[i], param.residual_type);  // stopping condition of solver
  }

  profile.TPSTOP(QUDA_PROFILE_PREAMBLE);
  profile.TPSTART(QUDA_PROFILE_COMPUTE);
  blas::flops = 0;

  int k = 0;

#ifdef BLOCKSOLVER_RELIABLE_POLICY_MIN
  double rNorm = 1e30; // reliable update policy is to use the smallest residual.
#else
  double rNorm = 0.0; // reliable update policy is to use the largest residual.
#endif

  PrintStats("Block-CG", k, r2avg / nsrc, b2avg, 0.);
  bool allconverged = true;
  bool converged[QUDA_MAX_BLOCK_SRC];
  for(int i=0; i<nsrc; i++){
    converged[i] = convergence(H(i,i).real(), 0., stop[i], param.tol_hq);
    allconverged = allconverged && converged[i];
#ifdef BLOCKSOLVER_RELIABLE_POLICY_MIN
    if (rNorm > sqrt(H(i,i).real())) rNorm = sqrt(H(i,i).real());
#else
    if (rNorm < sqrt(H(i,i).real())) rNorm = sqrt(H(i,i).real());
#endif
  }

  //double r0Norm = rNorm;
  double maxrx = rNorm;
  double maxrr = rNorm;
  double delta = param.delta;
  printfQuda("Reliable update delta = %.8f\n", delta);

  int rUpdate = 0;
  //int steps_since_reliable = 1;

  // Only matters for heavy quark residuals, which we don't have enabled
  // in blockCG (yet).
  //bool L2breakdown = false;

  // Step 6: L L^\dagger = H, Cholesky decomposition, L lower left triangular
  // Step 7: C = L^\dagger, C upper right triangular.
  // Set Linv = C.inverse() for convenience in the next step.
  L = H.llt().matrixL(); // retrieve factor L in the decomposition
  C = L.adjoint();
  Linv = C.inverse();

  #ifdef BLOCKSOLVER_VERBOSE
  std::cout << "r2\n " << H << std::endl;
  std::cout << "L\n " << L.adjoint() << std::endl;
  std::cout << "Linv = \n" << Linv << "\n";
  #endif

  // Step 8: finally set Q to thin QR decompsition of R.
  //blas::zero(*qp); // guaranteed to be zero at start.
#ifdef BLOCKSOLVER_MULTIREDUCE
  blas::copy(*tmpp, r); // Need to do this b/c r is fine, q is sloppy, can't caxpy w/ x fine, y sloppy.
  blas::caxpy_U(Linv_raw,tmpp->Components(),qp->Components());  // C is upper triangular, so its inverse is.
#else
  // temporary hack - use AC to pass matrix arguments to multiblas
  for(int i=0; i<nsrc; i++){
    for(int j=0;j<nsrc; j++){
      AC[i*nsrc + j] = Linv(i,j);
    }
  }
  blas::copy(*tmpp, r); // Need to do this b/c r is fine, q is sloppy, can't caxpy w/ x fine, y sloppy.
  blas::caxpy_U(AC,*tmpp,*qp); // C is upper triangular, so its inverse is.
#endif


  // Step 9: P = Q; additionally set P to thin QR decompoistion of r
  blas::copy(*pp, *qp);

#ifdef BLOCKSOLVER_VERBOSE
#ifdef BLOCKSOLVER_MULTIREDUCE
  blas::hDotProduct(pTp_raw, pp->Components(), pp->Components());
#else
  for(int i=0; i<nsrc; i++){
    for(int j=0; j<nsrc; j++){
      pTp(i,j) = blas::cDotProduct(pp->Component(i), pp->Component(j));
    }
  }
#endif

  std::cout << " pTp  " << std::endl << pTp << std::endl;
  std::cout << " L " << std::endl << L.adjoint() << std::endl;
  std::cout << " C " << std::endl << C << std::endl;
#endif

  // Step 10 was set S to the identity, but we took care of that
  // when we initialized all of the matrices. 

  bool just_reliable_updated = false; 
  while ( !allconverged && k < param.maxiter ) {
    // PUSH_RANGE("Dslash",1)
    //for(int i=0; i<nsrc; i++){
    // matSloppy(Ap.Component(i), p.Component(i), tmp.Component(i), tmp2.Component(i));  // tmp as tmp
    //}

    // Prepare to overlap some compute with comms.
    if (k > 0 && !just_reliable_updated)
    {
      dslash::aux_worker = &blockcg_update;
    }
    else
    {
      dslash::aux_worker = NULL;
      just_reliable_updated = false;
    }
    PUSH_RANGE("Dslash_sloppy",0)
    // Step 12: Compute Ap.
    matSloppy(Ap, *pp, tmp_matsloppy, tmp2);
    POP_RANGE

    PUSH_RANGE("Reduction",1)
    // Step 13: calculate pAp = P^\dagger Ap
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::hDotProduct_Anorm(pAp_raw, pp->Components(), Ap.Components());
#if 0
    // hermiticity check
    for(int i=0; i<nsrc; i++){
      for(int j=i; j < nsrc; j++){
        if ((fabs(pAp(i,j) - conj(pAp(j,i))) / fabs(pAp(i,j))) > 1e-2)
	  warningQuda("Violated i=%d j=%d %e %e %e %e", i, j, pAp(i,j).real(), pAp(i,j).imag(), pAp(j,i).real(), pAp(j,i).imag());
      }
    }
#endif
#else
    for(int i=0; i<nsrc; i++){
      for(int j=i; j < nsrc; j++){
        pAp(i,j) = blas::cDotProduct(pp->Component(i), Ap.Component(j));
        if (i!=j) pAp(j,i) = std::conj(pAp(i,j));
      }
    }
#endif
    POP_RANGE
    printmat("pAp", pAp);
    PUSH_RANGE("Eigen",3)
#ifdef BLOCKSOLVER_EXPLICIT_PAP_HERMITIAN
    H = 0.5*(pAp + pAp.adjoint().eval());
    pAp = H;
#endif

    // Step 14: Compute beta = pAp^(-1)
    // For convenience, we stick a minus sign on it now.
    beta = -pAp.inverse();

    // Step 15: Compute alpha = beta * C
    alpha = - beta * C;
    POP_RANGE
    // Step 16: update Xsloppy = Xsloppy + P alpha
    // This step now gets overlapped with the
    // comms in matSloppy. 
/*
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::caxpy(alpha_raw, *pp, x_sloppy);
#else
    // temporary hack using AC
    for(int i = 0; i < nsrc; i++){
      for(int j = 0; j < nsrc; j++){
        AC[i*nsrc + j] = alpha(i,j);
      }
    }
    blas::caxpy(AC,*pp,x_sloppy);
#endif
*/

    // Step 17: Update Q = Q - Ap beta (remember we already put the minus sign on beta)
    // update rSloppy
    PUSH_RANGE("BLAS",2)
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::caxpy(beta_raw, Ap, *qp);
#else
    // temporary hack
    for(int i=0; i<nsrc; i++){
      for(int j=0;j<nsrc; j++){
        AC[i*nsrc + j] = beta(i,j);
      }
    }
    blas::caxpy(AC,Ap,*qp);
#endif
    POP_RANGE

    PUSH_RANGE("Reduction",1)
    // Orthogonalize Q via a thin QR decomposition.
    // Step 18: H = Q^\dagger Q
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::hDotProduct(H_raw, qp->Components(), qp->Components());
#else
    printfQuda("Iteration %d\n",k);
    for(int i=0; i<nsrc; i++){
      for(int j=i; j < nsrc; j++){
        H(i,j) = blas::cDotProduct(qp->Component(i),qp->Component(j));
        //printfQuda("r2(%d,%d) = %.15e + I %.15e\n", i, j, real(r2(i,j)), imag(r2(i,j)));
        if (i!=j) H(j,i) = std::conj(H(i,j));
      }
    }
#endif
    printmat("r2", H);
  POP_RANGE
  PUSH_RANGE("Eigen",3)
    // Step 19: L L^\dagger = H; Cholesky decomposition of H, L is lower left triangular.
    L = H.llt().matrixL();// retrieve factor L  in the decomposition
    
    // Step 20: S = L^\dagger
    S = L.adjoint();

    // Step 21: Q = Q S^{-1}
    // This would be most "cleanly" implemented
    // with a block-cax, but that'd be a pain to implement.
    // instead, we accomplish it with a caxy, then a pointer
    // swap.

    Linv = S.inverse();
    blas::zero(*tmpp);
  POP_RANGE
  PUSH_RANGE("BLAS",2)
  blas::zero(*tmpp);
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::caxpy_U(Linv_raw, *qp, *tmpp); // tmp is acting as Q.
#else
    // temporary hack
    for(int i=0; i<nsrc; i++){
      for(int j=0;j<nsrc; j++){
        AC[i*nsrc + j] = Linv(i,j);
      }
    }
    blas::caxpy_U(AC,*qp,*tmpp); // tmp is acting as Q.
#endif
    POP_RANGE
    std::swap(qp, tmpp); // now Q actually is Q. tmp is the old Q.

    PUSH_RANGE("Eigen",3)
    // Step 22: Back up C (we need to have it if we trigger a reliable update)
    C_old = C;

    // Step 23: Update C. C = S * C_old. This will get overridden if we
    // trigger a reliable update.
    C = S * C;

    // Step 24: calculate the residuals for all shifts. We use these
    // to determine if we need to do a reliable update, and if we do,
    // these values get recomputed.
#ifdef BLOCKSOLVER_RELIABLE_POLICY_MIN
    double r2 = 1e30; // reliable update policy is to use the smallest residual.
#else
    double r2 = 0.0; // reliable update policy is to use the largest residual.
#endif

    r2avg=0;
    for (int j=0; j<nsrc; j++ ){
      H(j,j) = C(0,j)*conj(C(0,j));
      for(int i=1; i < nsrc; i++)
        H(j,j) += C(i,j) * conj(C(i,j));
      r2avg += H(j,j).real();
#ifdef BLOCKSOLVER_RELIABLE_POLICY_MIN
      if (r2 > H(j,j).real()) r2 = H(j,j).real();
  #else
      if (r2 < H(j,j).real()) r2 = H(j,j).real();
  #endif
    }
    POP_RANGE
#ifdef BLOCKSOLVER_EXPLICIT_QP_ORTHO
    bool did_reliable = false;
#endif
    if (block_reliable(rNorm, maxrx, maxrr, r2, delta))
    {
#ifdef BLOCKSOLVER_EXPLICIT_QP_ORTHO
      did_reliable = true;
#endif
      printfQuda("Triggered a reliable update on iteration %d!\n", k);
      // This was in the BiCGstab(-L) reliable updates, but I don't
      // think it's necessary... blas::xpy should support updating
      // y from a lower precision vector.
      //if (x.Precision() != x_sloppy.Precision())
      //{
      //  blas::copy(x, x_sloppy);
      //}

      // If we triggered a reliable update, we need
      // to do this X update now.
      // Step 16: update Xsloppy = Xsloppy + P alpha
      PUSH_RANGE("BLAS",1)
#ifdef BLOCKSOLVER_MULTIREDUCE
      blas::caxpy(alpha_raw, *pp, x_sloppy);
#else
      // temporary hack using AC
      for(int i = 0; i < nsrc; i++){
        for(int j = 0; j < nsrc; j++){
          AC[i*nsrc + j] = alpha(i,j);
        }
      }
      blas::caxpy(AC,*pp,x_sloppy);
#endif

      // Reliable updates step 2: Y = Y + X_s
      blas::xpy(x_sloppy, y);
      POP_RANGE
      // Don't do aux work!
      dslash::aux_worker = NULL;

      PUSH_RANGE("Dslash",4)
      // Reliable updates step 4: R = AY - B, using X as a temporary with the right precision.
      mat(r, y, x);
      POP_RANGE
      PUSH_RANGE("BLAS",2)
      blas::xpay(b, -1.0, r);

      // Reliable updates step 3: X_s = 0.
      // If x.Precision() == x_sloppy.Precision(), they refer
      // to the same pointer under the hood.
      // x gets used as a temporary in mat(r,y,x) above.
      // That's why we need to wait to zero 'x_sloppy' until here.
      blas::zero(x_sloppy);
      POP_RANGE
      // Reliable updates step 5: H = (R)^\dagger R
      r2avg=0;
      PUSH_RANGE("Reduction",1)
#ifdef BLOCKSOLVER_MULTIREDUCE
      blas::hDotProduct(H_raw, r.Components(), r.Components());
      for (int i = 0; i < nsrc; i++)
      {
        r2avg += H(i,i).real();
        printfQuda("r2[%i] %e\n", i, H(i,i).real());
      }
#else
      for(int i=0; i<nsrc; i++){
        for(int j=i; j < nsrc; j++){
          H(i,j) = blas::cDotProduct(r.Component(i),r.Component(j));
          if (i!=j) H(j,i) = std::conj(H(i,j));
          if (i==j) {
            r2avg += H(i,i).real();
            printfQuda("r2[%i] %e\n", i, H(i,i).real());
          }
        }
      }
#endif
      POP_RANGE
      PUSH_RANGE("Eigen",3)
      printmat("reliable r2", H);

      // Reliable updates step 6: L L^\dagger = H, Cholesky decomposition, L lower left triangular
      // Reliable updates step 7: C = L^\dagger, C upper right triangular.
      // Set Linv = C.inverse() for convenience in the next step.
      L = H.llt().matrixL(); // retrieve factor L in the decomposition
      C = L.adjoint();
      Linv = C.inverse();
      POP_RANGE

#ifdef BLOCKSOLVER_VERBOSE
      std::cout << "r2\n " << H << std::endl;
      std::cout << "L\n " << L.adjoint() << std::endl;
#endif

      PUSH_RANGE("BLAS",2)
      // Reliable updates step 8: set Q to thin QR decompsition of R.
      blas::zero(*qp);
#ifdef BLOCKSOLVER_MULTIREDUCE
      blas::copy(*tmpp, r); // Need to do this b/c r is fine, q is sloppy, can't caxpy w/ x fine, y sloppy.
      blas::caxpy_U(Linv_raw,*tmpp,*qp);
#else
      // temporary hack - use AC to pass matrix arguments to multiblas
      for(int i=0; i<nsrc; i++){
        for(int j=0;j<nsrc; j++){
          AC[i*nsrc + j] = Linv(i,j);
        }
      }
      blas::copy(*tmpp, r); // Need to do this b/c r is fine, q is sloppy, can't caxpy w/ x fine, y sloppy.
      blas::caxpy_U(AC,*tmpp,*qp); 
#endif
      POP_RANGE
      PUSH_RANGE("Eigen",3)

      // Reliable updates step 9: Set S = C * C_old^{-1} (equation 6.1 in the blockCGrQ paper)
      S = C * C_old.inverse();
      POP_RANGE

      // Reliable updates step 10: Recompute residuals, reset rNorm, etc.
#ifdef BLOCKSOLVER_RELIABLE_POLICY_MIN
      rNorm = 1e30; // reliable update policy is to use the smallest residual.
#else
      rNorm = 0.0; // reliable update policy is to use the largest residual.
#endif
      allconverged = true;
      for(int i=0; i<nsrc; i++){
        converged[i] = convergence(H(i,i).real(), 0., stop[i], param.tol_hq);
        allconverged = allconverged && converged[i];
#ifdef BLOCKSOLVER_RELIABLE_POLICY_MIN
        if (rNorm > sqrt(H(i,i).real())) rNorm = sqrt(H(i,i).real());
#else
        if (rNorm < sqrt(H(i,i).real())) rNorm = sqrt(H(i,i).real());
#endif
      }
      maxrx = rNorm;
      maxrr = rNorm;
      rUpdate++;

      just_reliable_updated = true; 

    } // end reliable.

    // Debug print of Q.
#ifdef BLOCKSOLVER_VERBOSE
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::hDotProduct(pTp_raw, qp->Components(), qp->Components());
#else
    for(int i=0; i<nsrc; i++){
      for(int j=0; j<nsrc; j++){
        pTp(i,j) = blas::cDotProduct(qp->Component(i), qp->Component(j));
      }
    }
#endif
    std::cout << " qTq " << std::endl << pTp << std::endl;
    std::cout <<  "QR" << S<<  std::endl << "QP " << S.inverse()*S << std::endl;;
#endif

    // Step 28: P = Q + P S^\dagger
    // This would be best done with a cxpay,
    // but that's difficult for the same
    // reason a block cax is difficult.
    // Instead, we do it by a caxpyz + pointer swap.

#ifdef BLOCKSOLVER_MULTIREDUCE
    PUSH_RANGE("Eigen",3)
    Sdagger = S.adjoint();
    POP_RANGE
    PUSH_RANGE("BLAS",2)
    blas::caxpyz_L(Sdagger_raw,*pp,*qp,*tmpp); // tmp contains P.
    POP_RANGE
#else
    PUSH_RANGE("BLAS",2)
    // temporary hack
    for(int i=0; i<nsrc; i++){
      for(int j=0;j<nsrc; j++){
        AC[i*nsrc + j] = std::conj(S(j,i));
      }
    }
    blas::caxpyz_L(AC,*pp,*qp,*tmpp); // tmp contains P.
    POP_RANGE
#endif
    std::swap(pp,tmpp); // now P contains P, tmp now contains P_old

    // Done with step 28.

#ifdef BLOCKSOLVER_EXPLICIT_QP_ORTHO
    if (did_reliable)
    {
      // Let's try to explicitly restore Q^\dagger P = I.
      Complex O_raw[nsrc*nsrc];
#ifdef BLOCKSOLVER_MULTIREDUCE
      Map<MatrixBCG> O(O_raw, nsrc, nsrc);
      blas::cDotProduct(O_raw, qp->Components(), pp->Components());
#else
      MatrixXcd O = MatrixXcd::Zero(nsrc, nsrc);
      for(int i=0; i<nsrc; i++){
        for(int j=0; j<nsrc; j++){
          O(i,j) = blas::cDotProduct(qp->Component(i), pp->Component(j));
        }
      }
#endif

      printfQuda("Current Q^\\dagger P:\n");
      std::cout << O << "\n";
      
#ifdef BLOCKSOLVER_MULTIREDUCE
      O -= MatrixBCG::Identity(nsrc,nsrc);
#else
      O -= MatrixXcd::Identity(nsrc,nsrc);
#endif
      O = -O;
      std::cout << "BLAH\n" << O << "\n";
#ifdef BLOCKSOLVER_MULTIREDUCE
      blas::caxpy(O_raw, *qp, *pp);
#else
      // temporary hack
      for(int i=0; i<nsrc; i++){
        for(int j=0;j<nsrc; j++){
          blas::caxpy(O(i,j),qp->Component(i),pp->Component(j));
        }
      }
#endif
      

      // Check...
#ifdef BLOCKSOLVER_MULTIREDUCE
      blas::cDotProduct(O_raw, qp->Components(), pp->Components());
#else
      for(int i=0; i<nsrc; i++){
        for(int j=0; j<nsrc; j++){
          O(i,j) = blas::cDotProduct(qp->Component(i), pp->Component(j));
        }
      }
#endif
      printfQuda("Updated Q^\\dagger P:\n");
      std::cout << O << "\n";
    }
    // End test...
#endif // BLOCKSOLVER_EXPLICIT_QP_ORTHO


#ifdef BLOCKSOLVER_VERBOSE
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::hDotProduct(pTp_raw, pp->Components(), pp->Components());
#else
    for(int i=0; i<nsrc; i++){
      for(int j=0; j<nsrc; j++){
        pTp(i,j) = blas::cDotProduct(pp->Component(i), pp->Component(j));
      }
    }
#endif

    std::cout << " pTp " << std::endl << pTp << std::endl;
    std::cout <<  "S " << S<<  std::endl << "C " << C << std::endl;
#endif

    k++;
    PrintStats("Block-CG", k, r2avg / nsrc, b2avg, 0);
    // Step 29: update the convergence check. H will contain the right
    // thing whether or not we triggered a reliable update.
    allconverged = true;
    for(int i=0; i<nsrc; i++){
      converged[i] = convergence(H(i,i).real(), 0, stop[i], param.tol_hq);
      allconverged = allconverged && converged[i];
    }


  }

  // Because we're overlapping communication w/ comms, 
  // x_sloppy doesn't get updated until the next iteration
  // (unless we happened ot hit a reliable update on the
  // last iteration).
  // However, we converged... so we never hit the next iteration.
  // We need to take care of this last update now.
  // Step ??: update Xsloppy = Xsloppy + P alpha
  // But remember tmpp holds the old P. 
  if (!just_reliable_updated)
  {
#ifdef BLOCKSOLVER_MULTIREDUCE
    blas::caxpy(alpha_raw, *tmpp, x_sloppy);
#else
    // temporary hack using AC
    for(int i = 0; i < nsrc; i++){
      for(int j = 0; j < nsrc; j++){
        AC[i*nsrc + j] = alpha(i,j);
      }
    }
    blas::caxpy(AC,*tmpp,x_sloppy);
#endif
  }

  // We've converged!
  // Step 27: Update Xs into Y.
  blas::xpy(x_sloppy, y);

  // And copy the final answer into X!
  blas::copy(x, y);

  profile.TPSTOP(QUDA_PROFILE_COMPUTE);
  profile.TPSTART(QUDA_PROFILE_EPILOGUE);

  param.secs = profile.Last(QUDA_PROFILE_COMPUTE);

  double gflops = (blas::flops + mat.flops() + matSloppy.flops())*1e-9;
  param.gflops = gflops;
  param.iter += k;

  { // temporary addition for SC'17
    comm_allreduce(&gflops);
    printfQuda("Block-CG(%d): Convergence in %d iterations, %f seconds, GFLOPS = %g\n", nsrc, k, param.secs, gflops / param.secs);
  }

  if (k == param.maxiter)
  warningQuda("Exceeded maximum iterations %d", param.maxiter);

  if (getVerbosity() >= QUDA_VERBOSE)
   printfQuda("Block-CG: Reliable updates = %d\n", rUpdate);

  dslash::aux_worker = NULL;

  if (param.compute_true_res) {
    // compute the true residuals
    mat(r, x, y, tmp3);
    for (int i=0; i<nsrc; i++){
      param.true_res = sqrt(blas::xmyNorm(b.Component(i), r.Component(i)) / b2[i]);
      param.true_res_hq = sqrt(blas::HeavyQuarkResidualNorm(x.Component(i), r.Component(i)).z);
      param.true_res_offset[i] = param.true_res;
      param.true_res_hq_offset[i] = param.true_res_hq;
    }
  }

  for (int i=0; i<nsrc; i++) {
    std::stringstream str;
    str << "Block-CG " << i;
    PrintSummary(str.str().c_str(), k, H(i,i).real(), b2[i]);
  }

  // reset the flops counters
  blas::flops = 0;
  mat.flops();
  matSloppy.flops();

  profile.TPSTOP(QUDA_PROFILE_EPILOGUE);
  profile.TPSTART(QUDA_PROFILE_FREE);

  if (&tmp3 != tmp_matsloppyp) delete tmp3_p;
  if (&tmp2 != tmp_matsloppyp) delete tmp2_p;

  profile.TPSTOP(QUDA_PROFILE_FREE);

  return;
}

#endif // BCGRQ
#endif // BLOCKSOVLER

void CG::solve(ColorSpinorField& x, ColorSpinorField& b) {

#ifndef BLOCKSOLVER
  errorQuda("QUDA_BLOCKSOLVER not built.");
#else

  if (param.num_src > QUDA_MAX_BLOCK_SRC)
    errorQuda("Requested number of right-hand sides %d exceeds max %d\n", param.num_src, QUDA_MAX_BLOCK_SRC);

  switch (param.num_src) {
  case  1: solve_n< 1>(x, b); break;
  case  2: solve_n< 2>(x, b); break;
  case  3: solve_n< 3>(x, b); break;
  case  4: solve_n< 4>(x, b); break;
  case  5: solve_n< 5>(x, b); break;
  case  6: solve_n< 6>(x, b); break;
  case  7: solve_n< 7>(x, b); break;
  case  8: solve_n< 8>(x, b); break;
  case  9: solve_n< 9>(x, b); break;
  case 10: solve_n<10>(x, b); break;
  case 11: solve_n<11>(x, b); break;
  case 12: solve_n<12>(x, b); break;
  case 13: solve_n<13>(x, b); break;
  case 14: solve_n<14>(x, b); break;
  case 15: solve_n<15>(x, b); break;
  case 16: solve_n<16>(x, b); break;
  case 24: solve_n<24>(x, b); break;
  case 32: solve_n<32>(x, b); break;
  case 48: solve_n<48>(x, b); break; 
  case 64: solve_n<64>(x, b); break;
  default:
    errorQuda("Block-CG with dimension %d not supported", param.num_src);
  }

#endif

}

}  // namespace quda
