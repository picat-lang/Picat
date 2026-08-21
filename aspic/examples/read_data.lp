%e(1,(2;3)).
%e(2,3).
%e(3,(4;5)).

%read the data from file instead
&cond((_={aspic_assert($e(X,Y)):A in read_file_lines('edges.txt').map(split),
                               B=A.map(to_int),X=B[1],Y=B[2]}
),1,1)&.

{sel(X,Y)}:-e(X,Y).

%enforce the global constraint
&cond((N=5,V=[{I,1}:I in 1..N],E=[{I,J,sel(I,J)}:I in 1..N,J in 1..N,I<J,e(I,J)==1],tree(V,E)),1,0)&.
