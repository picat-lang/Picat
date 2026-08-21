% https://potassco.org/clingo/run/?example

%doesn't produce a true stable set even with min ("asp" mode)

% instance
eagle(eddy).
penguin(tux).

% encoding
fly(X) :- bird(X), not nofly(X).
nofly(X) :- penguin(X).
bird(X) :- penguin(X).
bird(X) :- eagle(X).
