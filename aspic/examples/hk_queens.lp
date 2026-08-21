%
% N-queens in ASP.
%
% See http://en.wikipedia.org/wiki/Eight_queens_puzzle
%
% This was created by Hakan Kjellerstrand, hakank@bonetmail.com
% See also http://www.hakank.org/answer_set_programming/
%

#const n = 16.

% domain
number(1..n).


% alldifferent
1 { q(X,1..n) } 1 :- number(X).
1 { q(1..n,Y) } 1 :- number(Y).

% remove conflicting answers
% :- q(X1,Y1), q(X2,Y2), X1 < X2, Y1 == Y2.
:- q(X1,Y1), q(X2,Y2), X1 < X2, Y1 + X1 == Y2 + X2.
:- q(X1,Y1), q(X2,Y2), X1 < X2, Y1 - X1 == Y2 - X2.

#show q/2.
