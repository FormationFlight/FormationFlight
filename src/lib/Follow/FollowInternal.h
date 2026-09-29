#pragma once

#include "FollowManager.h"

// Free functions private to FollowManager.cpp's implementation. They are
// pure (no dependency on FollowManager's internal state) so they need no
// mock/fake seam of their own -- but pure logic still needs external
// linkage to be callable from a separate test translation unit, so they're
// declared here (rather than kept `static` inside the .cpp) purely so the
// native test suite can call them directly. FollowManager.cpp is still the
// only production caller, and this is not part of FollowManager's public API.

bool offsetGeometrySane(const FollowOffset &offset, double minSepM, double minVSepM, String *errMsg);

bool axisSignLocked(double candidateAxis, double referenceAxis,
                     double candidateOther1, double candidateOther2,
                     double referenceOther1, double referenceOther2,
                     double minSepM);

bool candidateOffsetOk(const FollowOffset &candidate, const FollowOffset &reference,
                        double minSepM, double minVSepM);

bool rcCandidateMatchesStaticDefault(const FollowOffset &candidate, const FollowRuntimeConfig &config);
