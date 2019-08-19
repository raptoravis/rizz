// A simple example to capture the following task dependencies.
//
// TaskA---->TaskB---->TaskD
// TaskA---->TaskC---->TaskD

#include <taskflow/taskflow.hpp>    // the only include you need


extern "C" int simple_example()
{

    tf::Executor executor;
    tf::Taskflow taskflow;

    auto [A, B, C, D] =
        taskflow.emplace([]() { std::cout << "TaskA\n"; },    //
                         []() { std::cout << "TaskB\n"; },    //          +---+
                         []() { std::cout << "TaskC\n"; },    //    +---->| B |-----+
                         []() { std::cout << "TaskD\n"; }     //    |     +---+     |
        );                                                    //  +---+           +-v-+
                                                              //  | A |           | D |
    A.precede(B);    // B runs after A         //  +---+           +-^-+
    A.precede(C);    // C runs after A         //    |     +---+     |
    B.precede(D);    // D runs after B         //    +---->| C |-----+
    C.precede(D);    // D runs after C         //          +---+

    executor.run(taskflow).wait();

    //executor.run(taskflow);                                           // run the taskflow once
    //executor.run(taskflow, []() { std::cout << "done 1 run\n"; });    // run once with a callback
    //executor.run_n(taskflow, 4);                                      // run four times
    //executor.run_n(taskflow, 4,
    //               []() { std::cout << "done 4 runs\n"; });    // run 4 times with a callback

    //int counter = 0;

    //// run n times until the predicate becomes true
    //counter = 4;
    //executor.run_until(taskflow, [&counter]() { return --counter == 0; });

    //// run n times until the predicate becomes true and invoke the callback on completion
    //counter = 4;
    //executor.run_until(taskflow, [&counter]() { return --counter == 0; },
    //                   []() { std::cout << "Execution finishes\n"; });


    //executor.wait_for_all();    // block until all associated tasks finish

    return 0;
}
