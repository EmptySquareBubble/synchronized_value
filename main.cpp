#include "synchronized_value.h"

#include <print>

struct cat {
    std::string name;
    int lives_cnt = 9;

    cat(std::string n) : name(std::move(n)) {}
    void say_it(int offset = 0) const 
    { 
        while(offset > 0)
        {
            std::print("--");
            --offset;
        }
        std::print("{} says meow\n", name); 
    }
    void set_name(const std::string& n) { name = n; }

    auto operator<=>(const cat& other) const {
        return lives_cnt <=> other.lives_cnt;
    }
};

int main() {

    //synchronized values encapsulates objects which are then accessed under the lock - locking is behind the scene
    synchronized_value<cat> liza{cat{"Liza"}};
    synchronized_value<cat> mourek{cat{"Mourek"}};
    synchronized_value<cat> pacicka{cat{"Pacicka"}};

    //you can access their functions/member directly 
    //operator -> is there to give you hint you are not dealing with naked type itself
    //everything will be accessed under the lock which is unlocked immediately
    liza->say_it();
    mourek->say_it();

    *liza = cat{"Zofie"};
    cat snizek = *mourek;
    liza->lives_cnt = 5;

    //comparisons make synchronized_scope internally
    if(snizek > liza)
        std::print("snizek has more lives than liza\n");
    else
        std::print("liza has more lives than snizek\n");
    
    if(mourek > liza)
        std::print("mourek has more lives than liza\n");
    else
        std::print("liza has more lives than mourek\n");
    liza->say_it();

    {
        //scope takes arbitrary number of synchronized_values and lock them during construction
        //during it's whole lifetime liza and mourek will be locked
        synchronized_scope scope(liza, mourek);
        
        //this access say_it directly
        liza->say_it(1);
        mourek->say_it(1);

        {
            synchronized_scope deeper_scope(mourek, pacicka); //mourek is already locked in this thread lock only pacicka
            liza->say_it(2);
            pacicka->say_it(2);
        }

        *mourek = cat{"Mourek Updated"};
        mourek->say_it(1);
    }

    liza->say_it();
    mourek->say_it();

    ///todo: mimic shared_mutex behavior
    return 0;
}