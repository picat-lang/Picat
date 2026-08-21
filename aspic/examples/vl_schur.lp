% code from https://www.cs.utexas.edu/~vl/teaching/378/ASP.pdf

% "in" changed to "a" to avoid clash with the reserved word in Picat
% order of the arguments changed, such that the partition is easier to get
% from the solution

#const n=45.
#const r=4.

% Partition {1 ,.. , n } into r sum - free subsets .
% input : positive integers n , r .
% a (K, I ) means that I belongs to the K - th subset .
{a(1..r,I)}=1 :- I=1..n.
% achieved : set {1 ,... , n } is partitioned into r subsets .
:-a(K,I),a(K,J),a(K,I +J).
% achieved : the subsets are sum - free .
