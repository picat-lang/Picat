%id, weight, value
item(1,3,15).
item(2,5,20).
item(3,2,14).
capacity(5).
{sel(I)}:-item(I,_,_).
:-capacity(C),#sum{W:sel(I),item(I,W,_)}>C.
%maximization using minimization
#minimize{0-V:sel(I),item(I,_,V)}.
