dom(1). dom(2). dom(3).
val(1,1). val(2,2). val(3,10).
{pick(X) : dom(X)}.
:~ not pick(1). [1000]
#maximize { V,X : pick(X), val(X,V) }.
