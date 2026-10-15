const nodemailer = require('nodemailer');
const config_module = require('./config');

/**
 * 创建发送邮件的代理
 */
let transport = nodemailer.createTransport({
    host: 'smtp.qq.com',
    port: 465,
    secure: true, // true for 465, false for other ports
    auth: {
        user: config_module.email_user, // generated ethereal user
        pass: config_module.email_pass, // generated ethereal password
    },
});

/**
 * 发送邮件
 * @param {*} mailOptions_ 发送邮件的参数
 * @returns 
 */

function sendEmail(mailOptions_) {
    return new Promise((resolve, reject) => {
        transport.sendMail(mailOptions_, (error, info) => {
            if (error) {
                console.log("sendEmail error:", error);
                reject(error);
            } else {
                console.log("sendEmail success:", info.response);
                resolve(info.response);
            }
        });
    });
}

module.exports.sendEmail = sendEmail;