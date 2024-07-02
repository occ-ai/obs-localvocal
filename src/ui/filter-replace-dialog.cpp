#include "filter-replace-dialog.h"
#include "ui_filter-replace-dialog.h"

FilterReplaceDialog::FilterReplaceDialog(QWidget *parent, transcription_filter_data *ctx_)
	: QDialog(parent),
	  ctx(ctx_),
	  ui(new Ui::FilterReplaceDialog)
{
	ui->setupUi(this);

	// populate the tableWidget with the filter_words_replace map
	ui->tableWidget->setRowCount((int)ctx->filter_words_replace.size());
	for (size_t i = 0; i < ctx->filter_words_replace.size(); i++) {
		const std::string key = std::get<0>(ctx->filter_words_replace[i]);
		const std::string value = std::get<1>(ctx->filter_words_replace[i]);
		ui->tableWidget->setItem((int)i, 0,
					 new QTableWidgetItem(QString::fromStdString(key)));
		ui->tableWidget->setItem((int)i, 1,
					 new QTableWidgetItem(QString::fromStdString(value)));
	}

	// connect toolButton_add
	connect(ui->toolButton_add, &QToolButton::clicked, this, &FilterReplaceDialog::addFilter);
	// connect toolButton_remove
	connect(ui->toolButton_remove, &QToolButton::clicked, this,
		&FilterReplaceDialog::removeFilter);
	// connect edit triggers
	connect(ui->tableWidget, &QTableWidget::itemChanged, this,
		&FilterReplaceDialog::editFilter);
}

FilterReplaceDialog::~FilterReplaceDialog()
{
	delete ui;
}

void FilterReplaceDialog::addFilter()
{
	ui->tableWidget->insertRow(ui->tableWidget->rowCount());
	// add an empty filter_words_replace map entry
	ctx->filter_words_replace.push_back(std::make_tuple("", ""));
}

void FilterReplaceDialog::removeFilter()
{
	if (ui->tableWidget->currentRow() == -1) {
		return;
	}
	ui->tableWidget->removeRow(ui->tableWidget->currentRow());
	// remove the filter_words_replace map entry
	ctx->filter_words_replace.erase(ctx->filter_words_replace.begin() +
					ui->tableWidget->currentRow() + 1);
}

void FilterReplaceDialog::editFilter(QTableWidgetItem *item)
{
	if (item->row() >= (int)ctx->filter_words_replace.size()) {
		return;
	}

	std::string key;
	if (ui->tableWidget->item(item->row(), 0) == nullptr) {
		key = "";
	} else {
		key = ui->tableWidget->item(item->row(), 0)->text().toStdString();
	}
	std::string value;
	if (ui->tableWidget->item(item->row(), 1) == nullptr) {
		value = "";
	} else {
		value = ui->tableWidget->item(item->row(), 1)->text().toStdString();
	}
	// use the row number to update the filter_words_replace map
	ctx->filter_words_replace[item->row()] = std::make_tuple(key, value);
}

std::string serialize_filter_words_replace(
	const std::vector<std::tuple<std::string, std::string>> &filter_words_replace)
{
	if (filter_words_replace.empty()) {
		return "[]";
	}
	// use JSON to serialize the filter_words_replace map
	nlohmann::json j;
	for (const auto &entry : filter_words_replace) {
		j.push_back({{"key", std::get<0>(entry)}, {"value", std::get<1>(entry)}});
	}
	return j.dump();
}

std::vector<std::tuple<std::string, std::string>>
deserialize_filter_words_replace(const std::string &filter_words_replace_str)
{
	if (filter_words_replace_str.empty()) {
		return {};
	}
	// use JSON to deserialize the filter_words_replace map
	std::vector<std::tuple<std::string, std::string>> filter_words_replace;
	nlohmann::json j = nlohmann::json::parse(filter_words_replace_str);
	for (const auto &entry : j) {
		filter_words_replace.push_back(std::make_tuple(entry["key"], entry["value"]));
	}
	return filter_words_replace;
}
