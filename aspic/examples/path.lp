%from https://personal.utdallas.edu/~gupta/nfm-ex/scasp-manual.pdf
edge(a,b).
edge(b,c).
edge(b,d).
edge(c,e).
edge(d,a).
path(X,Y) :- edge(X,Y).
path(X,Y) :- edge(X,Z),path(Z,Y).
