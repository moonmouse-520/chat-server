const fs = require('fs');
const path = require('path');

let config = JSON.parse(fs.readFileSync(path.join(__dirname, 'config.json'), 'utf8'));
let email_user = config.email.user;
let email_pass = config.email.pass;
let mysql_port = config.mysql.port;
let mysql_host = config.mysql.host;
let redis_port = config.redis.port;
let redis_host = config.redis.host;
let redis_password = config.redis.password;
let code_prefix = config.code.prefix;

module.exports = {
    email_user,
    email_pass,
    mysql_port,
    mysql_host,
    redis_port,
    redis_host,
    redis_password,
    code_prefix
}