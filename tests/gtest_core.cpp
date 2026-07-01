#include "MyProject/Account.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>
#include <vector>

// 用於範例死亡測試 (death test)
// EXPECT_DEATH 會檢查被測程式碼是否會崩潰，
// 並且其 stderr 輸出的內容是否符合指定的正則。
TEST(MyDeathTest, stdoutToStderr) {
  EXPECT_DEATH(
      {
        // 將 stdout 重導向 stderr，以便 EXPECT_DEATH 能夠捕捉到輸出
        dup2(STDERR_FILENO, STDOUT_FILENO);
        std::cout << "Normal log before crash";
        exit(1);
      },
      "Normal log.*");
}

// AccountTest 是一個測試 Fixture，負責初始化與清理 Account 物件。
class AccountTest : public ::testing::Test {
protected:
  void SetUp() override {
    acc = new Account("Test User", 500.0);
  }

  void TearDown() override {
    delete acc;
    acc = nullptr;
  }

  Account *acc = nullptr;
};

// 測試帳戶的基本存款、提款與餘額行為。
TEST_F(AccountTest, FullScenario) {
  EXPECT_DOUBLE_EQ(acc->getBalance(), 500.0);

  acc->deposit(100.0);
  EXPECT_DOUBLE_EQ(acc->getBalance(), 600.0);

  // 成功提款 200
  EXPECT_TRUE(acc->withdraw(200.0));
  EXPECT_DOUBLE_EQ(acc->getBalance(), 400.0);

  // 嘗試取款超過餘額，應該失敗且餘額不變
  EXPECT_FALSE(acc->withdraw(500.0));
  EXPECT_DOUBLE_EQ(acc->getBalance(), 400.0);
}

// VectorTest 透過 Fixture 保證每個測試都有獨立的 vector 初始狀態。
class VectorTest : public ::testing::Test {
protected:
  void SetUp() override {
    v = new std::vector<int>({1, 2, 3});
  }

  void TearDown() override {
    delete v;
    v = nullptr;
  }

  std::vector<int> *v = nullptr;
};

// 測試 A：清空後 vector 會變成空的。
TEST_F(VectorTest, ClearEmpty) {
  v->clear();
  EXPECT_TRUE(v->empty());
}

// 測試 B：原始 vector 含有 3 個元素，push_back 後大小變成 4。
TEST_F(VectorTest, PushBackSize) {
  v->push_back(4);
  EXPECT_EQ(v->size(), 4);
}

// 以下是參數化測試範例，使用同一個 Fixture 針對多組輸入執行相同測試。
class IsEvenTest : public ::testing::TestWithParam<int> {};

TEST_P(IsEvenTest, CheckEvenNumbers) {
  int n = GetParam();
  EXPECT_TRUE(n % 2 == 0) << n << " is not even";
}

TEST_P(IsEvenTest, CheckDivisibleByThree) {
  int n = GetParam();
  EXPECT_TRUE(n % 3 == 0) << n << " is not divisible by 3";
}

INSTANTIATE_TEST_SUITE_P(ThreeSequence, IsEvenTest,
                         ::testing::Values(6, 12, 18, 24));

// 簡單的 API 介面與 Service 類別範例，用於展示 Mock。
class Api {
public:
  virtual ~Api() = default;
  virtual int fetch(int id) = 0;
};

template <typename ApiType>
class Service {
public:
  explicit Service(ApiType *api) : api_(api) {}
  int getData(int id) { return api_->fetch(id); }

private:
  ApiType *api_;
};

class MockApi {
public:
  MOCK_METHOD(int, fetch, (int id), ());
};

TEST(ServiceTest, FetchData) {
  MockApi mock;

  // 先後期望 fetch(42) 兩次，依序回傳 100 與 200
  EXPECT_CALL(mock, fetch(42))
      .WillOnce(::testing::Return(100))
      .WillOnce(::testing::Return(200));
  EXPECT_CALL(mock, fetch(64)).WillOnce(::testing::Return(300));

  Service<MockApi> service(&mock);
  ASSERT_EQ(service.getData(42), 100);
  ASSERT_EQ(service.getData(64), 300);
  ASSERT_EQ(service.getData(42), 200);
}
