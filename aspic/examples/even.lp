%populates the universe
{n(0..10)}.
% e(X) iff natural X is even
e(0).
e(N):-e(N-2). %clingo never stops grounding this
%e(N):-N=2..10,e(N-2). %clingo works with this

