"""Minimal disposable accounts required by unchanged endpoint DDL mysqltest.

Never reuse or overwrite an existing admin/test_ai_user on any host. The caller
must run only on its isolated test instance and invoke cleanup even if setup fails.
"""
import secrets


class EndpointAccounts:
    def __init__(self, execute):
        self.execute = execute
        self.password = secrets.token_hex(24)
        self.admin_owned = False
        self.case_started = False

    def setup(self):
        count = self.execute("SELECT COUNT(*) FROM oceanbase.__all_user "
                             "WHERE user_name IN ('admin','test_ai_user')")
        if count.strip() != '0':
            raise RuntimeError('admin or test_ai_user already exists; refusing to reuse any host account')
        self.execute("CREATE USER 'admin'@'127.0.0.1' IDENTIFIED BY '" + self.password + "'")
        self.admin_owned = True
        self.execute("GRANT CREATE USER ON *.* TO 'admin'@'127.0.0.1'")
        self.execute("GRANT SELECT ON test.* TO 'admin'@'127.0.0.1' WITH GRANT OPTION")
        # SHOW GRANTS for another user checks mysql.user SELECT.
        self.execute("GRANT SELECT ON mysql.user TO 'admin'@'127.0.0.1'")
        for privilege in ['CREATE AI MODEL', 'ALTER AI MODEL', 'DROP AI MODEL']:
            self.execute("GRANT " + privilege + " ON *.* TO 'admin'@'127.0.0.1' WITH GRANT OPTION")

    def cleanup(self):
        errors = []
        if self.admin_owned:
            accounts = ["'test_ai_user'@'%'"] if self.case_started else []
            accounts.append("'admin'@'127.0.0.1'")
            for account in accounts:
                try:
                    self.execute('DROP USER IF EXISTS ' + account)
                except Exception as error:
                    errors.append(str(error).replace(self.password, '<ephemeral-password>'))
            if not errors:
                self.admin_owned = False
        if errors:
            raise RuntimeError('account cleanup failed: ' + '; '.join(errors))
