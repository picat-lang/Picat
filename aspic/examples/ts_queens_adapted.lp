% very fast ASP n-queens, personal communication Prof. Torsten Schaub
% not that fast yet with ASPIC
% slightly modified
% explicit parantheses to workaround the parsing issues in arithmetic expressions
#const n=128.
c(1..n).
{numbers(1..n*2-1)}. %populate the universe
{ queen(I,1..n) } = 1 :- I = 1..n.
{ queen(1..n,J) } = 1 :- J = 1..n.

 :- { queen(I,J) : c(I),c(J),d1(I,J,D) } > 1, D=1..n*2-1.
 :- { queen(I,J) : c(I),c(J),d2(I,J,D) } > 1, D=1..n*2-1.

d1(I,J,(I-J)+n) :- I = 1..n, J = 1..n.
d2(I,J,(I+J-1)) :- I = 1..n, J = 1..n.

#show queen/2.
