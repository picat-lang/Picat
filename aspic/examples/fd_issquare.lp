
is_square(E):-E=0..1024,
&cond(
S=fd(x,6)
,S*S#=E,0)&.

a:-is_square(E).

:- not a.
