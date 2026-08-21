%proper way to embed CP FD variables and constraints:
% S#=E (constraint that S and E are equal) is not enforced,
% it is just made equivalent to the head of the rule, "eq"
eq:-
&cond((
S=fd(s,2),
E=fd(e,2)
),S#=E,0)&.

