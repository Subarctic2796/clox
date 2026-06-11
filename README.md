# Clox
[Crafting Interpreters](https://craftinginterpreters.com) bytecode vm.

# Running
```console
git clone --depth=1 https://github.com/Subarctic2796/clox.git
cd clox
make build
./clox
```

I have made quite a few additions
- multiline comments
- arrays and maps
- indexing
- lambdas
- break and continue statements
- for in loops
- errors as values
- range objects
- Semi native iterators

## Arrays and maps and indexing
```lox
var arr = [1, "hello", nil, fun() { return "this is from a lambda"; }];
print arr; // [1, "hello", nil, <fn fun>]
print arr[1]; // hello

var map = {1: nil, "key": fun() { print "another lambda"; }};
print map; // {key: <fn fun>, 1: nil}
print map["key"]; // <fn fun>
```

## For in loops and Iterators
You can now iterable over iterable objects (strings, arrays, maps, ranges, and custom iterators).
```lox
// var iter = [1, 2, 3, 4];
// var iter = {"key1": 23, nil: fun(){print "a lambda";}};
// var iter = range(0, 11, 2);
var iter = "hello world";
for (var ix, i in iter) {
    print ix;
    print i;
}

/*
you can also create custom iterators
to do this you create a class that the methods
- next : is the iterator still valid
- index: the index, for eg in maps this is the key
- value: the value, for eg in maps this is the value
you should also make your iterator inherit from the native class `Iter`
but you don't have to
*/

// we are making an iterator for a linked list
class Node {
    init(val) {
        this.val = val;
        this.next = nil;
    }
}

class LinkedList {
    init() { this.head = nil; }
    add(val) {
        if (this.head == nil) {
            this.head = Node(val);
            return;
        }
        var last = this.head;
        while (last.next != nil) {
            last = last.next;
        }
        last.next = Node(val);
    }
}

// The actual linked list iterator
// Its best practice to inherit from the native `Iter` class
class LinkedListIter < Iter {
    init(ll) {
        this.list = ll;
        this.val = nil;
    }

    // returns a bool saying if the iterator is still valid or not
    // by convention it should also advance the iterator
    next() {
        var result = this.list.head != nil;
        if (result) {
            this.val = this.list.head.val;
            this.list.head = this.list.head.next;
        }
        return result;
    }

    // returns the index of the iterator
    // if that doesn't make sense then you should return nil
    // or whatever you like
    index() { return nil; }

    // returns the value of the iterator
    value() { return this.val; }
}

var list = LinkedList();
for (var i = 0; i < 5; i = i + 1) {
    list.add(i);
}

for (var ix, i in LinkedListIter(list)) {
    print ix;
    print i;
}
```

## Lambdas
```lox
var lambda = fun(a) { print a; return a + 1; };
var ret = lambda(23);
print ret; // 23
```

## Break and continue statements
```lox
for (var i = 0; i < 10; i = i + 1) {
    if (i == 3) continue;
    if (i == 7) break;
    print i;
}
// extected output 1 2 4 5 6
```

## errors as values
For now you need to use the native error function to create a new error.
```lox
fun oldEnough(age) {
    if (age < 18) return error("age is less than 18");
    print "you may enter";
    return true;
}

var age = 17;
var allowed = oldEnough(age);
if (typeof(allowed) == "ERROR") {
    print allowed;
} else {
    print "welcome";
}
```
