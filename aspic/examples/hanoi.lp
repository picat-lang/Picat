#const n=256.
#const k=8.

p(1..3). %towers
x(1..k). %discs
at(1,1,1..k). %initial
:-not at(n,3,1..k). %final
{at(T,P,X):p(P)}=1:-T=1..n,x(X).
{move(T,P1,P2,X):p(P1),p(P2),at(T,P1,X),P1!=P2}=1:-T=1..n-1.
at(T,P2,X):-T=2..n,p(P2),p(P),move(T-1,P,P2,X).
at(T,P,X):-T=2..n,p(P),at(T-1,P,X),not move(T-1,X).
move(T,X):-x(X),p(P1),p(P2),move(T,P1,P2,X). %projection
movefrom(T,P,X):-p(P),p(Q),x(X),move(T,P,Q,X). %projection
moveto(T,Q,X):-p(P),p(Q),x(X),move(T,P,Q,X). %projection
% cannot place over a smaller one
:-p(P2),moveto(T,P2,X),at(T,P2,Y),Y<X. 
% can only move the smallest one
:-p(P),x(X),movefrom(T,P,X),at(T,P,Y),Y<X. 

#show move/4.
