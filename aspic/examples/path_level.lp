% correct even under just Clark completion
num(1..5).
edge(a,b).
edge(b,c).
edge(b,d).
edge(c,e).
edge(d,a).
path(X,Y,1) :- edge(X,Y).
path(X,Y,N) :- num(N),edge(X,Z),path(Z,Y,N-1).
