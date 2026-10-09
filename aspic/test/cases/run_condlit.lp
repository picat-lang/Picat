dom(1). dom(2). dom(3).
{ p(X) : dom(X) }.
all :- p(X) : dom(X).
:- not all.
