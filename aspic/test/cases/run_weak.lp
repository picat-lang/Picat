dom(1). dom(2). dom(3).
val(1,1). val(2,2). val(3,10).
{pick(X) : dom(X)}.
:~ not pick(1), not pick(2), not pick(3). [1000]
:~ pick(X),val(X,V). [V,X]
:~ pick(1). [2]
