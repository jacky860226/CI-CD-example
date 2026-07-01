#include "MyProject/Account.hpp"
#include "ScopeTimer.hpp"

#include <iostream>

int main() {
    ScopeTimer timer("Main function");
    Account acc("John Doe", 1000.0);

    std::cout << "Initial balance: " << acc.getBalance() << std::endl;

    acc.deposit(500.0);
    std::cout << "After deposit: " << acc.getBalance() << std::endl;

    if (acc.withdraw(200.0)) {
        std::cout << "After withdrawal: " << acc.getBalance() << std::endl;
    } else {
        std::cout << "Withdrawal failed." << std::endl;
    }

    return 0;
}
