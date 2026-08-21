n(0..9).
sol(S,E,N,D,M,O,R,Y):-n(S),n(E),n(N),n(D),n(M),n(O),n(R),n(Y),
S!=0,M!=0,
S*1000+E*100+N*10+D+M*1000+O*100+R*10+E=M*10000+O*1000+N*100+E*10+Y,
% #count{S;E;N;D;M;O;R;Y}=8
S!=E,S!=N,S!=D,S!=M,S!=O,S!=R,S!=Y,
E!=N,E!=D,E!=M,E!=O,E!=R,E!=Y,
N!=D,N!=M,N!=O,N!=R,N!=Y,
D!=M,D!=O,D!=R,D!=Y,
M!=O,M!=R,M!=Y,
O!=R,O!=Y,
R!=Y
.

% #show sol/8.
