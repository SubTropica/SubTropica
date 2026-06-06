// Data-driven MPFR bridge for IterInt: read iterated-integral words (high-precision
// rational dlog letters), evaluate each via the Boost/MPFR backend, print high-prec
// values. Keeps the rational prefactor assembly on the Mathematica side.
//
// Robustness (2026-06-03 hardening):
//   - each word is evaluated inside a try/catch; on any C++ exception (e.g. odeint
//     "could not find a step size" when a letter sits ON the integration path) the
//     word emits "nan nan" instead of aborting the whole process. This guarantees
//     EXACTLY one output line per input word, so the Mathematica side can assert
//     Length[vals]==Length[words] and see precisely which words failed.
//   - stdout is flushed after every word, so a later hard failure never loses the
//     lines already computed.
//   - non-finite results (inf/nan in either component) are normalised to "nan nan".
//   - a per-word diagnostic ("word k: <what happened>") is written to stderr.
//
// stdin format:
//   <prec_digits> <method> <tolExp> <hStartExp> <switchExp>
//     method: 0 = plainIntegrate, 1 = splittingPlainIntegrate
//             (method 0 is REQUIRED for trailing-zero words; method 1 corrupts them)
//   <nWords>
//   for each word:  <z_re> <z_im> <nLetters>
//                   then nLetters lines (Goncharov order a1..an):  <a_re> <a_im> <residue>
//                   (residue is 1 iff the letter is exactly 0, else 0)
// stdout: one line per word:  <re> <im>   (prec_digits significant figures), or "nan nan"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <boost/multiprecision/mpfr.hpp>
#include <boost/multiprecision/mpc.hpp>
#include "integratorBoost.hpp"

namespace mp = boost::multiprecision;
using nr    = mp::mpfr_float;
using cmplx = mp::mpc_complex;

int main() {
  // config line: prec method tolExp hStartExp switchExp
  long prec, method, tolExp, hStartExp, switchExp;
  if (!(std::cin >> prec >> method >> tolExp >> hStartExp >> switchExp)) return 1;
  nr::default_precision(prec);
  cmplx::default_precision(prec);
  std::cout << std::setprecision(prec);

  nr epsA   = nr("1e-" + std::to_string(tolExp));
  nr epsR   = nr("1e-" + std::to_string(tolExp));
  nr hStart = nr("1e-" + std::to_string(hStartExp));
  nr swP    = nr("1e-" + std::to_string(switchExp));
  iteratedIntegrals::BoostIntegrator<nr, cmplx> integrator(
      epsA, epsR, hStart, nr{1}, nr{0} /*startRegulator*/, swP /*switchPoint*/, cmplx{1, 0});

  int nWords;
  if (!(std::cin >> nWords)) return 1;
  for (int w = 0; w < nWords; ++w) {
    std::string zre, zim;
    std::cin >> zre >> zim;
    cmplx z{nr(zre), nr(zim)};
    int nL;
    std::cin >> nL;
    std::vector<cmplx> avals(nL);
    std::vector<int>   ares(nL);
    for (int i = 0; i < nL; ++i) {
      std::string are, aim; int res;
      std::cin >> are >> aim >> res;
      avals[i] = cmplx{nr(are), nr(aim)};
      ares[i]  = res;
    }
    // IterInt list is innermost-first = reverse of the Goncharov index list
    std::vector<iteratedIntegrals::Integrand<cmplx>> kernels;
    kernels.reserve(nL);
    for (int i = nL - 1; i >= 0; --i) {
      cmplx a = avals[i];
      kernels.emplace_back(cmplx{static_cast<long>(ares[i])},
                           [a](const cmplx& x) {
                             if (x == a) return cmplx{};
                             return cmplx{1} / (x - a);
                           });
    }

    bool ok = true;
    std::string why;
    cmplx res{};
    try {
      res = (nL == 0) ? cmplx{1}
          : (method == 1) ? integrator.splittingPlainIntegrate(z, kernels)
                          : integrator.plainIntegrate(z, kernels);
      // reject non-finite components (pole grazed on the path, overflow, ...)
      using mp::isnan;
      using mp::isinf;
      nr re = res.real(), im = res.imag();
      if (isnan(re) || isnan(im) || isinf(re) || isinf(im)) { ok = false; why = "non-finite result"; }
    } catch (const std::exception& e) {
      ok = false; why = std::string("exception: ") + e.what();
    } catch (...) {
      ok = false; why = "unknown exception";
    }

    if (ok) {
      std::cout << res.real() << " " << res.imag() << "\n" << std::flush;
      std::cerr << "word " << w << ": ok\n";
    } else {
      std::cout << "nan nan\n" << std::flush;
      std::cerr << "word " << w << ": FAILED (" << why << ")\n";
    }
  }
  return 0;
}
