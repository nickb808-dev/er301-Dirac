/* libdirac.swig — SWIG interface for the Dirac ER-301 Object. */

%module dirac_libdirac

%include <od/glue/mod.cpp.swig>

%{
#undef SWIGLUA
#include <Dirac.h>
#include <DiracHeadDisplay.h>
#include <DiracFieldGraphic.h>
#define SWIGLUA
using namespace dirac;
%}

%include <Dirac.h>
%include <DiracHeadDisplay.h>
%include <DiracFieldGraphic.h>
