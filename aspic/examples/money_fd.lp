main:-
&cond((
S=fd(s,4),
E=fd(e,4),
N=fd(n,4),
D=fd(d,4),
M=fd(m,4),
O=fd(o,4),
R=fd(r,4),
Y=fd(y,4),
[E,N,D,O,R,Y]::0..9,
[S,M]::1..9,
	          (1000 * S + 100 * E + 10 * N + D) +
					  (1000 * M + 100 * O + 10 * R + E) #=
(10000 * M + 1000 * O + 100 * N + 10 * E + Y),
all_different([S,E,N,D,M,O,R,Y])
),1,1)&.

% :-not main.

