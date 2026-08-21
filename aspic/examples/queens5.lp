% from https://potassco.org/clingo/run/?example
% aspic
% times with completion on AMD EPYC 9654
% 128    0.9 s
% 256    3.2 s
% 512   26.2 s
% 768   89.3 s
% 1024 944.6 s

#const n = 128.


% n-Queens encoding

{ q(I,1..n) } == 1 :- I = 1..n.
{ q(1..n,J) } == 1 :- J = 1..n.
:- { q(D-J,J) } >= 2, D = 2..2*n.
:- { q(D+J,J) } >= 2, D = 1-n..n-1.
