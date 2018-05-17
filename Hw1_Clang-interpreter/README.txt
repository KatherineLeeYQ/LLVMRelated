Requirements: Implement a basic interpreter based on Clang

Marking: 25 testcases are provided and each test case counts for 1 mark

Supported Language: We support a subset of C language constructs, as follows: 

Type: int | char
Operator: * | + | - | * | / | < | > | == | = | [ ] 
Statements: IfStmt | WhileStmt | ForStmt | DeclStmt 
Expr : BinaryOperator | UnaryOperator | DeclRefExpr | CallExpr | CastExpr 

We also need to support 4 external functions int GET(), int * MALLOC(int), void FREE (int *), void PRINT(int), the semantics of the 4 funcions are self-explanatory. 

A skelton implemnentation ast-interpreter.tgz is provided, and you are welcome to make any changes to the implementation. The provided implementation is able to interpreter the simple program like : 

extern int GET();
extern int MALLOC(int);
extern void FREE(int);
extern void PRINT(int);

int main() {
   int a;
   a = GET();
   PRINT(a);
}