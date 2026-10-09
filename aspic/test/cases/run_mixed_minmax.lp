dom(1). dom(2). dom(3).
val(1,1). val(2,2). val(3,10).
{pick(X) : dom(X)}.
{q}.
#maximize { V,X : pick(X), val(X,V) }.
#minimize { 1 : q }.
